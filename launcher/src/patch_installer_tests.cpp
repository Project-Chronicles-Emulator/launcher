#include "patch_installer.hpp"

#include <gtest/gtest.h>
#include <windows.h>

#include <atomic>
#include <algorithm>
#include <fstream>
#include <map>

namespace chronicles::launcher {
namespace {

using namespace distribution;
using json = nlohmann::json;

void write_text(const std::filesystem::path &path, const std::string &text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << text;
  ASSERT_TRUE(output.good());
}

class PatchInstallTest : public testing::Test {
protected:
  void SetUp() override {
    static std::atomic<unsigned> fixture_number{};
    fixture_root_ = std::filesystem::temp_directory_path() /
                    ("chronicles-patch-test-" + std::to_string(GetCurrentProcessId()) + "-" +
                     std::to_string(GetTickCount64()) + "-" + std::to_string(++fixture_number));
    ASSERT_TRUE(std::filesystem::create_directory(fixture_root_));
    game_ = fixture_root_ / "game";
  }

  void TearDown() override {
    const auto resolved = std::filesystem::canonical(fixture_root_);
    ASSERT_EQ(resolved.parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    ASSERT_TRUE(resolved.filename().string().starts_with("chronicles-patch-test-"));
    std::error_code error;
    std::filesystem::remove_all(resolved, error);
    EXPECT_FALSE(error) << error.message();
  }

  Release release(unsigned sequence, const std::string &content) {
    Release result;
    result.channel = "stable";
    result.version = "fixture-" + std::to_string(sequence);
    result.sequence = sequence;
    result.api_origin = "http://127.0.0.1:8480";
    result.payload_sha256 = sha256_bytes(result.version + content);
    for (const auto &path : {"Game/first.bin", "Game/second.bin"}) {
      result.files.push_back({path, sha256_bytes(content), content.size()});
      result.total_bytes += content.size();
    }
    contents_[sha256_bytes(content)] = content;
    return result;
  }

  void install(const Release &release, const std::function<void()> &guard = [] {}) {
    PatchInstaller(game_).install(
        release,
        [&](const auto &file, const auto &destination, const auto &, auto) {
          write_text(destination, contents_.at(file.sha256));
          ++downloads_;
        },
        [](auto, auto, const auto &, const auto &) {}, {}, guard);
  }

  std::filesystem::path fixture_root_;
  std::filesystem::path game_;
  std::map<std::string, std::string> contents_;
  unsigned downloads_{};
};

TEST_F(PatchInstallTest, InstallsRepairsAndPreservesUnlistedPlayerFiles) {
  const auto first = release(1, "verified first content");
  install(first);
  EXPECT_TRUE(PatchInstaller(game_).inspect(first).changed.empty());
  write_text(game_ / "player-notes.txt", "Keep this user file");
  write_text(game_ / "Game/first.bin", "corrupted");
  ASSERT_EQ(PatchInstaller(game_).inspect(first).changed.size(), 1);
  install(first);
  EXPECT_EQ(sha256_file(game_ / "Game/first.bin"), first.files[0].sha256);
  EXPECT_EQ(read_bounded_file(game_ / "player-notes.txt", 100), "Keep this user file");
  EXPECT_EQ(downloads_, 1);
}

TEST_F(PatchInstallTest, RealmSwitchKeepsIndependentRollbackHistory) {
  const auto live = release(10, "same verified game bytes");
  install(live);
  auto local = release(2, "same verified game bytes");
  local.api_origin = "http://127.0.0.1:8080";
  local.payload_sha256 = sha256_bytes("local catalog");
  EXPECT_NO_THROW(install(local));
  EXPECT_TRUE(PatchInstaller(game_).inspect(live).changed.empty());

  auto older_live = live;
  older_live.sequence = 9;
  older_live.payload_sha256 = sha256_bytes("old live catalog");
  EXPECT_THROW(PatchInstaller(game_).inspect(older_live), std::runtime_error);
  EXPECT_NO_THROW(install(live));
  EXPECT_THROW(PatchInstaller(game_).inspect(release(1, "old bytes")), std::runtime_error);
}

TEST_F(PatchInstallTest, RejectsUnmanagedFoldersAndReleaseDowngrades) {
  const auto first = release(1, "original");
  const auto second = release(2, "replacement");
  write_text(game_ / "existing-game.bin", "An installation we do not own");
  EXPECT_THROW(PatchInstaller(game_).inspect(first), std::runtime_error);
  std::filesystem::remove(game_ / "existing-game.bin");
  install(second);
  EXPECT_THROW(PatchInstaller(game_).inspect(first), std::runtime_error);
  auto conflict = second;
  conflict.payload_sha256 = sha256_bytes("conflicting same sequence");
  EXPECT_THROW(PatchInstaller(game_).inspect(conflict), std::runtime_error);
}

TEST_F(PatchInstallTest, AdoptsExistingGameAndOnlyReplacesChangedSignedFiles) {
  const auto signed_release = release(1, "verified content");
  write_text(game_ / "X5/Binaries/Win64/ArcheAgeChronicles.exe", "existing client");
  write_text(game_ / "Game/first.bin", "verified content");
  write_text(game_ / "Game/second.bin", "old game bytes");
  write_text(game_ / "player-notes.txt", "preserve this file");

  PatchInstaller installer(game_, ExistingDirectory::game);
  const auto plan = installer.inspect(signed_release);
  ASSERT_EQ(plan.changed.size(), 1);
  EXPECT_EQ(plan.changed.front().path, "Game/second.bin");

  installer.install(
      signed_release,
      [&](const auto &file, const auto &destination, const auto &, auto) {
        write_text(destination, contents_.at(file.sha256));
        ++downloads_;
      },
      [](auto, auto, const auto &, const auto &) {}, {}, [] {});

  EXPECT_EQ(downloads_, 1);
  EXPECT_EQ(read_bounded_file(game_ / "Game/first.bin", 100), "verified content");
  EXPECT_EQ(read_bounded_file(game_ / "Game/second.bin", 100), "verified content");
  EXPECT_EQ(read_bounded_file(game_ / "player-notes.txt", 100), "preserve this file");
  EXPECT_TRUE(installer.is_current_release(signed_release));
}

TEST_F(PatchInstallTest, LauncherAdoptionPreservesExistingUserFiles) {
  const auto signed_release = release(1, "new launcher content");
  write_text(game_ / "Game/first.bin", "old launcher content");
  write_text(game_ / "runtime/settings.json", "preserve settings");

  PatchInstaller installer(game_, ExistingDirectory::launcher);
  installer.install(
      signed_release,
      [&](const auto &file, const auto &destination, const auto &, auto) {
        write_text(destination, contents_.at(file.sha256));
      },
      [](auto, auto, const auto &, const auto &) {}, {}, [] {});

  EXPECT_EQ(read_bounded_file(game_ / "Game/first.bin", 100), "new launcher content");
  EXPECT_EQ(read_bounded_file(game_ / "runtime/settings.json", 100), "preserve settings");
  EXPECT_TRUE(installer.inspect(signed_release).changed.empty());
}

TEST_F(PatchInstallTest, ColocatedLauncherAndGameKeepIndependentUpdateMetadata) {
  const auto game_release = release(1, "game bytes");
  auto launcher_release = release(1, "launcher bytes");
  launcher_release.channel = "launcher-stable";
  launcher_release.files[0].path = "launcher/first.bin";
  launcher_release.files[1].path = "launcher/second.bin";
  write_text(game_ / "X5/Binaries/Win64/ArcheAgeChronicles.exe", "existing client");

  const auto download = [&](const auto &file, const auto &destination, const auto &, auto) {
    write_text(destination, contents_.at(file.sha256));
  };
  const auto progress = [](auto, auto, const auto &, const auto &) {};
  PatchInstaller game_installer(game_, ExistingDirectory::game);
  PatchInstaller launcher_installer(game_, ExistingDirectory::launcher);
  game_installer.install(game_release, download, progress, {}, [] {});
  const auto game_marker = read_bounded_file(game_ / ".chronicles-install.json", 65536);
  launcher_installer.install(launcher_release, download, progress, {}, [] {});

  EXPECT_EQ(read_bounded_file(game_ / ".chronicles-install.json", 65536), game_marker);
  EXPECT_TRUE(game_installer.is_current_release(game_release));
  EXPECT_TRUE(launcher_installer.is_current_release(launcher_release));
  EXPECT_TRUE(std::filesystem::exists(game_ / ".chronicles-launcher-install.json"));
  EXPECT_TRUE(game_installer.inspect(game_release).changed.empty());
  EXPECT_TRUE(launcher_installer.inspect(launcher_release).changed.empty());

  write_text(launcher_update_journal_path(game_), "invalid launcher journal");
  EXPECT_TRUE(game_installer.is_current_release(game_release));
  EXPECT_FALSE(launcher_installer.is_current_release(launcher_release));
  EXPECT_THROW(launcher_installer.recover_pending(), std::exception);
  EXPECT_TRUE(game_installer.inspect(game_release).changed.empty());
}

TEST_F(PatchInstallTest, ExistingGameCanBeAdoptedAfterColocatedLauncherUpdate) {
  const auto game_release = release(1, "game bytes");
  auto launcher_release = release(1, "launcher bytes");
  launcher_release.channel = "launcher-stable";
  launcher_release.files[0].path = "launcher/first.bin";
  launcher_release.files[1].path = "launcher/second.bin";
  write_text(game_ / "X5/Binaries/Win64/ArcheAgeChronicles.exe", "existing client");

  const auto download = [&](const auto &file, const auto &destination, const auto &, auto) {
    write_text(destination, contents_.at(file.sha256));
  };
  const auto progress = [](auto, auto, const auto &, const auto &) {};
  PatchInstaller launcher_installer(game_, ExistingDirectory::launcher);
  launcher_installer.install(launcher_release, download, progress, {}, [] {});
  const auto launcher_marker = read_bounded_file(game_ / ".chronicles-launcher-install.json", 65536);

  PatchInstaller game_installer(game_, ExistingDirectory::game);
  game_installer.install(game_release, download, progress, {}, [] {});
  EXPECT_EQ(read_bounded_file(game_ / ".chronicles-launcher-install.json", 65536), launcher_marker);
  EXPECT_TRUE(game_installer.is_current_release(game_release));
  EXPECT_TRUE(launcher_installer.is_current_release(launcher_release));

  write_text(game_ / ".chronicles-update/journal.json", "invalid game journal");
  EXPECT_FALSE(game_installer.is_current_release(game_release));
  EXPECT_TRUE(launcher_installer.is_current_release(launcher_release));
  EXPECT_TRUE(launcher_installer.inspect(launcher_release).changed.empty());
}

TEST_F(PatchInstallTest, RecoversLauncherJournalWithoutTouchingColocatedGame) {
  const auto game_release = release(1, "game bytes");
  auto old_launcher = release(1, "old launcher bytes");
  old_launcher.channel = "launcher-stable";
  old_launcher.files[0].path = "launcher/first.bin";
  old_launcher.files[1].path = "launcher/second.bin";
  auto new_launcher = release(2, "new launcher bytes");
  new_launcher.channel = "launcher-stable";
  new_launcher.files[0].path = old_launcher.files[0].path;
  new_launcher.files[1].path = old_launcher.files[1].path;
  write_text(game_ / "X5/Binaries/Win64/ArcheAgeChronicles.exe", "existing client");

  const auto download = [&](const auto &file, const auto &destination, const auto &, auto) {
    write_text(destination, contents_.at(file.sha256));
  };
  const auto progress = [](auto, auto, const auto &, const auto &) {};
  PatchInstaller game_installer(game_, ExistingDirectory::game);
  PatchInstaller launcher_installer(game_, ExistingDirectory::launcher);
  game_installer.install(game_release, download, progress, {}, [] {});
  launcher_installer.install(old_launcher, download, progress, {}, [] {});
  const auto game_marker = read_bounded_file(game_ / ".chronicles-install.json", 65536);
  const auto launcher_marker_path = game_ / ".chronicles-launcher-install.json";
  const auto previous = json::parse(read_bounded_file(launcher_marker_path, 65536));
  auto next = previous;
  next["sequence"] = new_launcher.sequence;
  next["payload_sha256"] = new_launcher.payload_sha256;
  json entries = json::array();
  for (const auto &file : new_launcher.files) {
    const auto backup = game_ / ".chronicles-launcher-update/transactions" /
                        new_launcher.payload_sha256 / "backup" / file.path;
    write_text(backup, "old launcher bytes");
    entries.push_back({{"path", file.path},
                       {"sha256", file.sha256},
                       {"size", file.size},
                       {"old_sha256", old_launcher.files[0].sha256}});
  }
  write_text(game_ / new_launcher.files[0].path, "new launcher bytes");
  write_text(launcher_update_journal_path(game_),
             json{{"application", "chronicles"},
                  {"payload_sha256", new_launcher.payload_sha256},
                  {"previous", previous},
                  {"next", next},
                  {"files", entries}}
                 .dump());

  launcher_installer.recover_pending();
  EXPECT_EQ(read_bounded_file(game_ / new_launcher.files[0].path, 100), "old launcher bytes");
  EXPECT_FALSE(std::filesystem::exists(launcher_update_journal_path(game_)));
  EXPECT_EQ(read_bounded_file(game_ / ".chronicles-install.json", 65536), game_marker);
  EXPECT_TRUE(game_installer.is_current_release(game_release));
  EXPECT_TRUE(launcher_installer.is_current_release(old_launcher));
}

TEST_F(PatchInstallTest, AdoptingAnExistingGameRollsBackACommitFailure) {
  const auto signed_release = release(1, "replacement");
  write_text(game_ / "X5/Binaries/Win64/ArcheAgeChronicles.exe", "existing client");
  write_text(game_ / "Game/first.bin", "first original");
  write_text(game_ / "Game/second.bin", "second original");

  const auto locked_path = game_ / "Game/second.bin";
  HANDLE lock = CreateFileW(locked_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(lock, INVALID_HANDLE_VALUE);
  EXPECT_THROW(PatchInstaller(game_, ExistingDirectory::game).install(
                   signed_release,
                   [&](const auto &file, const auto &destination, const auto &, auto) {
                     write_text(destination, contents_.at(file.sha256));
                   },
                   [](auto, auto, const auto &, const auto &) {}, {}, [] {}),
               std::runtime_error);
  CloseHandle(lock);

  EXPECT_EQ(read_bounded_file(game_ / "Game/first.bin", 100), "first original");
  EXPECT_EQ(read_bounded_file(game_ / "Game/second.bin", 100), "second original");
  EXPECT_FALSE(std::filesystem::exists(game_ / ".chronicles-update/journal.json"));
}

TEST_F(PatchInstallTest, ExistingGameAdoptionRequiresTheClientExecutable) {
  const auto signed_release = release(1, "content");
  write_text(game_ / "unrelated.txt", "keep");
  EXPECT_THROW(PatchInstaller(game_, ExistingDirectory::game).inspect(signed_release),
               std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(game_ / ".chronicles-install.json"));
}

TEST_F(PatchInstallTest, CorruptDownloadNeverReplacesInstalledFiles) {
  const auto first = release(1, "original");
  const auto second = release(2, "replacement");
  install(first);
  contents_[second.files[0].sha256] = "tampered blob";
  EXPECT_THROW(install(second), std::runtime_error);
  EXPECT_EQ(sha256_file(game_ / "Game/first.bin"), first.files[0].sha256);
  EXPECT_FALSE(std::filesystem::exists(game_ / ".chronicles-update/journal.json"));
}

TEST_F(PatchInstallTest, CommitFailureRollsBackAlreadyReplacedFiles) {
  const auto first = release(1, "original");
  const auto second = release(2, "replacement");
  install(first);
  const auto locked_path = game_ / "Game/second.bin";
  HANDLE lock = CreateFileW(locked_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(lock, INVALID_HANDLE_VALUE);
  EXPECT_THROW(install(second), std::runtime_error);
  CloseHandle(lock);
  EXPECT_EQ(sha256_file(game_ / "Game/first.bin"), first.files[0].sha256);
  EXPECT_EQ(sha256_file(game_ / "Game/second.bin"), first.files[1].sha256);
  EXPECT_EQ(json::parse(read_bounded_file(game_ / ".chronicles-install.json", 65536))["sequence"],
            1);
  EXPECT_FALSE(std::filesystem::exists(game_ / ".chronicles-update/journal.json"));
}

TEST_F(PatchInstallTest, RecoversAnInterruptedCommitBeforePlanningTheNextUpdate) {
  const auto first = release(1, "original");
  const auto second = release(2, "replacement");
  install(first);
  const auto previous = json::parse(read_bounded_file(game_ / ".chronicles-install.json", 65536));
  auto next = previous;
  next["sequence"] = 2;
  next["version"] = second.version;
  next["payload_sha256"] = second.payload_sha256;
  json entries = json::array();
  for (const auto &file : second.files) {
    const auto backup =
        game_ / ".chronicles-update/transactions" / second.payload_sha256 / "backup" / file.path;
    write_text(backup, "original");
    entries.push_back({{"path", file.path},
                       {"sha256", file.sha256},
                       {"size", file.size},
                       {"old_sha256", first.files[0].sha256}});
  }
  write_text(game_ / "Game/first.bin", "replacement");
  write_text(game_ / ".chronicles-update/journal.json",
             json{{"application", "chronicles"},
                  {"payload_sha256", second.payload_sha256},
                  {"previous", previous},
                  {"next", next},
                  {"files", entries}}
                 .dump());
  EXPECT_EQ(PatchInstaller(game_).inspect(second).changed.size(), 2);
  EXPECT_EQ(read_bounded_file(game_ / "Game/first.bin", 100), "original");
  EXPECT_FALSE(std::filesystem::exists(game_ / ".chronicles-update/journal.json"));
}

TEST_F(PatchInstallTest, GameRunningGuardAndCancellationPreventInstallation) {
  const auto first = release(1, "original");
  EXPECT_THROW(install(first, [] { throw std::runtime_error("Game is running"); }),
               std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(game_ / "Game/first.bin"));
  std::stop_source cancellation;
  cancellation.request_stop();
  EXPECT_THROW(PatchInstaller(game_).install(
                   first, {}, [](auto, auto, const auto &, const auto &) {},
                   cancellation.get_token(), [] {}),
               std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(game_ / "Game/first.bin"));
}

TEST_F(PatchInstallTest, CancelDuringInspectionPreservesInstalledFilesAndReleaseMarker) {
  const auto first = release(1, "original game files");
  install(first);
  const auto marker = read_bounded_file(game_ / ".chronicles-install.json", 65536);
  std::stop_source cancellation;
  unsigned progress_calls = 0;

  EXPECT_THROW(PatchInstaller(game_).inspect(first, cancellation.get_token(),
                                             [&](auto, auto, const auto &, const auto &) {
                                               ++progress_calls;
                                               cancellation.request_stop();
                                             }),
               std::runtime_error);
  EXPECT_EQ(progress_calls, 1);
  EXPECT_EQ(read_bounded_file(game_ / ".chronicles-install.json", 65536), marker);
  EXPECT_EQ(read_bounded_file(game_ / "Game/first.bin", 100), "original game files");
  EXPECT_EQ(sha256_file(game_ / "Game/first.bin"), first.files.front().sha256);
  EXPECT_THROW(sha256_file(game_ / "Game/first.bin", cancellation.get_token()), std::runtime_error);
}

TEST_F(PatchInstallTest, FastStartupRequiresVerifiedFilesAndPreservesRollbackProtection) {
  const auto first = release(1, "original");
  install(first);
  EXPECT_TRUE(PatchInstaller(game_).is_current_release(first));

  auto different_origin = first;
  different_origin.api_origin = "https://other-realm.example";
  EXPECT_TRUE(PatchInstaller(game_).is_current_release(different_origin));
  auto different_payload = first;
  different_payload.payload_sha256 = std::string(64, 'b');
  EXPECT_FALSE(PatchInstaller(game_).is_current_release(different_payload));

  write_text(game_ / "Game/first.bin", "short");
  EXPECT_FALSE(PatchInstaller(game_).is_current_release(first));
  write_text(game_ / "Game/first.bin", "modified");
  // Startup checks installation metadata; explicit verification detects same-size edits.
  EXPECT_TRUE(PatchInstaller(game_).is_current_release(first));
  EXPECT_EQ(PatchInstaller(game_).inspect(first).changed.size(), 1);
  write_text(game_ / ".chronicles-update/journal.json", "pending transaction");
  EXPECT_FALSE(PatchInstaller(game_).is_current_release(first));
}

TEST_F(PatchInstallTest, SwitchingRealmsReusesTheSameSignedFileSet) {
  const auto local = release(2, "same game content");
  install(local);

  auto marker = json::parse(read_bounded_file(game_ / ".chronicles-install.json", 65536));
  marker.erase("files_sha256");
  write_text(game_ / ".chronicles-install.json", marker.dump());

  auto live = local;
  live.api_origin = "https://other-realm.example";
  live.payload_sha256 = sha256_bytes("different signed realm catalog");
  PatchInstaller installer(game_);
  EXPECT_FALSE(installer.is_current_release(live));
  ASSERT_TRUE(installer.is_current_release(local));
  installer.remember_current_release(local);

  ASSERT_TRUE(installer.is_current_release(live));
  installer.remember_current_release(live);
  EXPECT_TRUE(installer.is_current_release(local));
  installer.remember_current_release(local);

  auto conflicting_live = live;
  conflicting_live.payload_sha256 = sha256_bytes("conflicting same sequence");
  EXPECT_FALSE(installer.is_current_release(conflicting_live));
  EXPECT_THROW(installer.inspect(conflicting_live), std::runtime_error);

  auto changed_live = live;
  ++changed_live.sequence;
  changed_live.payload_sha256 = sha256_bytes("new signed realm catalog");
  changed_live.files.front().sha256 = sha256_bytes("different game content");
  EXPECT_FALSE(installer.is_current_release(changed_live));
}

TEST_F(PatchInstallTest, InspectionReportsProgressInsideALargeExistingFile) {
  const std::string content(20 * 1024 * 1024, 'x');
  Release signed_release;
  signed_release.channel = "stable";
  signed_release.version = "fixture-large-file";
  signed_release.sequence = 1;
  signed_release.api_origin = "http://127.0.0.1:8480";
  signed_release.payload_sha256 = sha256_bytes("large-file-release");
  signed_release.total_bytes = content.size();
  signed_release.files.push_back(
      {"X5/Content/Paks/X5-Windows.ucas", sha256_bytes(content), content.size()});
  write_text(game_ / signed_release.files.front().path, content);
  write_text(game_ / ".chronicles-install.json",
             json{{"application", "chronicles"},
                  {"channel", "stable"},
                  {"sequence", 1},
                  {"version", signed_release.version},
                  {"payload_sha256", signed_release.payload_sha256}}
                 .dump());

  std::vector<std::uint64_t> reported_bytes;
  const auto plan = PatchInstaller(game_).inspect(
      signed_release, {}, [&](std::uint64_t completed, std::uint64_t total,
                              const std::string &path, const std::string &phase) {
        EXPECT_EQ(total, content.size());
        EXPECT_EQ(path, signed_release.files.front().path);
        EXPECT_EQ(phase, "checking");
        reported_bytes.push_back(completed);
      });

  EXPECT_TRUE(plan.changed.empty());
  EXPECT_TRUE(std::ranges::any_of(reported_bytes, [&](std::uint64_t completed) {
    return completed > 0 && completed < content.size();
  }));
  EXPECT_EQ(reported_bytes.back(), content.size());
}

TEST_F(PatchInstallTest, FastStartupRejectsSameSizeSteamCompatibilityChange) {
  auto signed_release = release(1, "original");
  signed_release.files.front().path =
      "Engine/Binaries/ThirdParty/Steamworks/Steamv161/Win64/steam_api64.dll";
  install(signed_release);
  EXPECT_TRUE(PatchInstaller(game_).is_current_release(signed_release));

  write_text(game_ / signed_release.files.front().path, "modified");

  EXPECT_FALSE(PatchInstaller(game_).is_current_release(signed_release));
  ASSERT_EQ(PatchInstaller(game_).inspect(signed_release).changed.size(), 1);
}

} // namespace
} // namespace chronicles::launcher
