#include "controller.hpp"
#include "game_compatibility.hpp"
#include "game_discovery.hpp"
#include "release_servers.hpp"
#include "settings.hpp"

#include <gtest/gtest.h>
#include <windows.h>

#include <filesystem>
#include <fstream>

namespace chronicles::launcher {
namespace {

class ReleaseSetupTest : public testing::Test {
protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("chronicles-release-setup-" + std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(GetTickCount64()));
    ASSERT_TRUE(std::filesystem::create_directory(root_));
    target_prefix_ = L"Project Chronicles Controller Test/" + root_.filename().wstring() + L"/";
  }

  void TearDown() override {
    const auto resolved = std::filesystem::canonical(root_);
    ASSERT_EQ(resolved.parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    ASSERT_TRUE(resolved.filename().string().starts_with("chronicles-release-setup-"));
    std::filesystem::remove_all(resolved);
  }

  void edition(const std::string &name) {
    nlohmann::json profile{{"edition", name}};
    if (name == "operator") {
      profile["servers"] = nlohmann::json::array({
          {{"id", "local"}, {"name", "Local testing"},
           {"server_url", "http://127.0.0.1:8080"}},
          {{"id", "live-na"}, {"name", "Hosted test realm"},
           {"server_url", "https://api.chronicles.example"},
           {"world_host", "world.chronicles.example"},
           {"patch_public_key", std::string(64, 'a')}}});
    }
    std::ofstream output(root_ / "launcher-config.json");
    output << profile.dump();
  }

  std::filesystem::path root_;
  std::wstring target_prefix_;
};

TEST(ValveLibraryDiscovery, ParsesEscapedLibrariesWithoutStartingSteam) {
  const auto paths = valve_values(R"vdf(
        "libraryfolders" {
            "0" { "path" "C:\\Program Files (x86)\\Steam" "apps" { "5280930" "123" } }
            "1" { "path" "E:\\SteamLibrary" }
        }
    )vdf",
                                  "path");
  ASSERT_EQ(paths.size(), 2);
  EXPECT_EQ(paths[0], "C:\\Program Files (x86)\\Steam");
  EXPECT_EQ(paths[1], "E:\\SteamLibrary");
  EXPECT_THROW(valve_values("\"path\" \"unfinished", "path"), std::invalid_argument);
}

TEST_F(ReleaseSetupTest, FindsCompletedManagedGameAndIgnoresIncompleteCandidates) {
  const auto incomplete = root_ / "Chronicles-A" / "runtime/game";
  const auto completed = root_ / "Chronicles-B" / "runtime/game";
  for (const auto &game : {incomplete, completed}) {
    const auto executable = game / "X5/Binaries/Win64/ArcheAgeChronicles.exe";
    std::filesystem::create_directories(executable.parent_path());
    std::ofstream(executable) << "client";
  }
  {
    std::ofstream marker(incomplete / ".chronicles-install.json");
    marker << R"({"application":"chronicles","channel":"stable","sequence":0})";
  }
  {
    std::ofstream marker(completed / ".chronicles-install.json");
    marker << nlohmann::json{{"application", "chronicles"},
                              {"channel", "stable"},
                              {"sequence", 2},
                              {"payload_sha256", std::string(64, 'a')}}.dump();
  }

  EXPECT_EQ(find_managed_game({root_}, "stable"), completed);
  EXPECT_FALSE(find_managed_game({root_}, "other").has_value());
}

TEST_F(ReleaseSetupTest, FallsBackToAnotherExistingGameWhenNoManagedGameExists) {
  const auto game = root_ / "ArcheAge-copy";
  const auto executable = game / "X5/Binaries/Win64/ArcheAgeChronicles.exe";
  std::filesystem::create_directories(executable.parent_path());
  std::ofstream(executable) << "client";

  EXPECT_EQ(find_other_game({root_}, "stable"), game);
  std::ofstream(game / ".chronicles-install.json") << "invalid marker";
  EXPECT_FALSE(find_other_game({root_}, "stable").has_value());
}


TEST_F(ReleaseSetupTest, PreservesExistingSelectionBeforeSteamDiscovery) {
  const auto selected_game = root_ / "selected-game";
  const auto steam_game = root_ / "steam-game";
  const auto executable = selected_game / "X5/Binaries/Win64/ArcheAgeChronicles.exe";
  std::filesystem::create_directories(executable.parent_path());
  std::ofstream(executable) << "client";

  const auto settings_directory = root_ / "settings";
  std::filesystem::create_directories(settings_directory);
  {
    std::ofstream settings_file(settings_directory / "settings.json");
    settings_file << nlohmann::json{{"game_directory", selected_game.string()}}.dump();
  }
  const auto legacy_settings = SettingsStore(settings_directory).load();
  EXPECT_FALSE(legacy_settings.game_directory_explicit);
  const auto preferred = preferred_game_directory(
      legacy_settings.game_directory, legacy_settings.game_directory_explicit, steam_game);
  ASSERT_TRUE(preferred.has_value());
  EXPECT_EQ(*preferred, selected_game);

  const auto missing_game = root_ / "missing-game";
  EXPECT_EQ(preferred_game_directory(missing_game, false, steam_game), steam_game);
  EXPECT_EQ(preferred_game_directory(missing_game, true, steam_game), missing_game);
  EXPECT_FALSE(preferred_game_directory(missing_game, false, std::nullopt).has_value());
}

TEST_F(ReleaseSetupTest, DefaultCatalogSelectsOnlyTheLocalServer) {
  edition("player");
  const auto servers = release_servers(root_);
  ASSERT_EQ(servers.size(), 1);
  EXPECT_TRUE(servers.front().local);
  EXPECT_TRUE(servers.front().patch_public_key.empty());

  LauncherSettings settings;
  settings.server_id = "removed-hosted-server";
  EXPECT_TRUE(apply_distribution_profile(settings, root_));
  EXPECT_EQ(settings.server_id, "local");
  EXPECT_EQ(settings.server_url, "http://127.0.0.1:8080");
  EXPECT_EQ(settings.world_host, "127.0.0.1");
  EXPECT_EQ(settings.world_port, 8081);
  EXPECT_EQ(settings.client_launch_target, "steam");
  EXPECT_TRUE(settings.auto_redirect);
  EXPECT_TRUE(settings.auto_world_entry);
}

TEST_F(ReleaseSetupTest, UnsignedLocalCatalogPreservesAnExplicitPrivateClient) {
  edition("player");
  {
    std::ofstream profile(root_ / "launcher-config.json");
    profile << R"({"edition":"player","servers":[{"id":"local","name":"Local test","server_url":"http://127.0.0.1:18080","world_port":18081}]})";
  }
  LauncherSettings settings;
  settings.client_launch_target = "private_copy";

  ASSERT_TRUE(apply_distribution_profile(settings, root_));
  EXPECT_EQ(settings.client_launch_target, "private_copy");
  EXPECT_EQ(settings.launch_mode, "named_account");
  EXPECT_EQ(settings.http_port, 18080);
  EXPECT_EQ(settings.world_port, 18081);
  EXPECT_TRUE(settings.auto_redirect);
  EXPECT_FALSE(settings.auto_start_server);
}

TEST_F(ReleaseSetupTest, HostedCatalogKeepsSignedManagedDistribution) {
  edition("operator");
  LauncherSettings settings;
  settings.server_id = "live-na";
  settings.client_launch_target = "private_copy";

  ASSERT_TRUE(apply_distribution_profile(settings, root_));
  EXPECT_EQ(settings.client_launch_target, "managed");
  EXPECT_FALSE(settings.patch_public_key.empty());
  EXPECT_FALSE(settings.auto_redirect);
}

TEST_F(ReleaseSetupTest, HostedCatalogRequiresAnExplicitSigningKeyAndValidWorldPort) {
  nlohmann::json profile{
      {"edition", "operator"},
      {"servers", nlohmann::json::array({
          {{"id", "hosted"}, {"name", "Hosted test realm"},
           {"server_url", "https://realm.example"}}})}};
  const auto save = [&] {
    std::ofstream output(root_ / "launcher-config.json");
    output << profile.dump();
  };

  save();
  EXPECT_THROW(release_servers(root_), std::invalid_argument);
  profile["servers"][0]["patch_public_key"] = std::string(64, 'a');
  profile["servers"][0]["world_port"] = 70000;
  save();
  EXPECT_THROW(release_servers(root_), std::invalid_argument);
  profile["servers"][0]["world_port"] = 4294967297ULL;
  save();
  EXPECT_THROW(release_servers(root_), std::invalid_argument);
  profile["servers"][0]["world_port"] = 8081.5;
  save();
  EXPECT_THROW(release_servers(root_), std::invalid_argument);
  profile["servers"][0]["world_port"] = 8443;
  save();
  const auto servers = release_servers(root_);
  ASSERT_EQ(servers.size(), 1);
  EXPECT_EQ(servers.front().world_host, "realm.example");
  EXPECT_EQ(servers.front().world_port, 8443);
}

TEST_F(ReleaseSetupTest, PlayerNativePolicyRefusesOperatorLoopbackConfiguration) {
  edition("operator");
  LauncherSettings settings;
  settings.server_id = "local";
  settings.game_directory = root_ / "runtime" / "game";
  settings.game_directory_explicit = true;
  SettingsStore(root_ / "out/launcher").save(settings);
  LauncherController player(root_, root_, true, {}, target_prefix_);
  EXPECT_THROW(player.probe(), std::invalid_argument);
  EXPECT_THROW(player.select_server("local"), std::invalid_argument);
}

TEST_F(ReleaseSetupTest, OperatorCatalogSupportsPersistedLocalSelection) {
  edition("operator");
  LauncherSettings settings;
  settings.server_id = "local";
  EXPECT_TRUE(apply_distribution_profile(settings, root_));
  EXPECT_EQ(release_servers(root_).size(), 2);
  EXPECT_EQ(settings.server_url, "http://127.0.0.1:8080");
  EXPECT_EQ(settings.world_host, "127.0.0.1");
  EXPECT_EQ(settings.world_port, 8081);

  SettingsStore store(root_ / "settings");
  store.save(settings);
  const auto restored = store.load();
  EXPECT_EQ(restored.server_id, "local");
  EXPECT_EQ(restored.world_host, "127.0.0.1");
}

TEST_F(ReleaseSetupTest, OfflineStartupPublishesCatalogAndAllowsImmediateServerSwitch) {
  edition("operator");
  LauncherSettings settings;
  settings.server_id = "local";
  settings.game_directory = root_ / "runtime" / "game";
  settings.game_directory_explicit = true;
  SettingsStore(root_ / "out/launcher").save(settings);

  LauncherController controller(root_, root_, false, {}, target_prefix_);
  std::vector<nlohmann::json> states;
  controller.set_state_sink(
      [&](const std::string &state) { states.push_back(nlohmann::json::parse(state)); });

  const auto started = std::chrono::steady_clock::now();
  controller.initialize();
  ASSERT_FALSE(states.empty());
  EXPECT_EQ(states.front().at("settings").at("server_id"), "local");
  EXPECT_EQ(states.front().at("release_servers").size(), 2);
  EXPECT_FALSE(states.front().at("server").at("running"));

  controller.select_server("live-na");
  EXPECT_EQ(states.back().at("settings").at("server_id"), "live-na");
  EXPECT_EQ(states.back().at("server").at("url"), "https://api.chronicles.example");
  EXPECT_FALSE(states.back().at("server").at("running"));
  EXPECT_EQ(SettingsStore(root_ / "out/launcher").load().server_id, "live-na");
  // Neither initializing the view nor selecting a realm should contact a server.
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
}

TEST_F(ReleaseSetupTest, ServerSwitchRestoresEachWindowsCredentialWithoutSigningItOut) {
  edition("operator");
  const auto legacy_path = root_ / "out/launcher/login.dat";
  CredentialStore store(legacy_path, target_prefix_);

  SavedLogin local;
  local.origin = "http://127.0.0.1:8080";
  local.account = "localuser";
  local.token = std::string(64, 'a');
  local.keep_signed_in = true;
  store.save(local);

  SavedLogin live;
  live.origin = "https://api.chronicles.example";
  live.account = "liveuser";
  live.token = std::string(64, 'b');
  live.keep_signed_in = true;
  store.save(live);

  LauncherSettings settings;
  settings.server_id = "local";
  settings.game_directory = root_ / "runtime" / "game";
  settings.game_directory_explicit = true;
  SettingsStore(root_ / "out/launcher").save(settings);
  LauncherController controller(root_, root_, false, {}, target_prefix_);
  controller.initialize();
  EXPECT_EQ(controller.state().at("auth").at("account"), "localuser");

  controller.select_server("live-na");
  EXPECT_EQ(controller.state().at("auth").at("account"), "liveuser");
  controller.select_server("local");
  EXPECT_EQ(controller.state().at("auth").at("account"), "localuser");

  store.erase(local.origin);
  store.erase(live.origin);
}

TEST_F(ReleaseSetupTest, ViewCanRequestTheStartupErrorAfterInitializationFailed) {
  edition("operator");
  LauncherSettings settings;
  settings.server_id = "local";
  settings.game_directory = root_ / "runtime" / "game";
  settings.game_directory_explicit = true;
  SettingsStore(root_ / "out/launcher").save(settings);
  unsigned health_requests = 0;
  LauncherController player(
      root_, root_, true,
      [&](const std::string &) -> nlohmann::json {
        ++health_requests;
        throw std::runtime_error("Synthetic unavailable realm");
      },
      target_prefix_);

  try {
    player.initialize();
    FAIL() << "The native player policy must reject a local profile";
  } catch (const std::invalid_argument &error) {
    player.note_error(error.what());
  }

  nlohmann::json delivered;
  player.set_state_sink(
      [&](const std::string &state) { delivered = nlohmann::json::parse(state); });
  player.publish_state();
  ASSERT_TRUE(delivered.is_object());
  EXPECT_FALSE(delivered.at("error").get<std::string>().empty());
  ASSERT_EQ(delivered.at("release_servers").size(), 1);
  EXPECT_EQ(delivered.at("release_servers").front().at("id"), "live-na");

  player.select_server("live-na");
  player.refresh();
  EXPECT_EQ(health_requests, 1);
  EXPECT_EQ(player.state().at("settings").at("server_id"), "live-na");
}

TEST_F(ReleaseSetupTest, SavedLoginCanBeRestoredBeforeAnyNetworkRequest) {
  const auto credentials = root_ / "login.dat";
  const auto target_prefix = L"Project Chronicles Release Test/" + root_.filename().wstring() + L"/";
  SavedLogin saved;
  saved.origin = "https://realm.example";
  saved.account = "alice";
  saved.token = std::string(64, 'a');
  saved.keep_signed_in = true;
  CredentialStore(credentials, target_prefix).save(saved);

  unsigned requests = 0;
  AccountSession session(
      [&](const auto &, const auto &, const auto &, const auto &) {
        ++requests;
        return nlohmann::json::object();
      },
      {}, credentials, target_prefix);
  session.restore_saved_login(saved.origin, saved.account, false);
  EXPECT_EQ(requests, 0);
  EXPECT_FALSE(session.state().at("connected"));

  session.refresh();
  EXPECT_EQ(requests, 1);
  EXPECT_TRUE(session.state().at("connected"));
  CredentialStore(credentials, target_prefix).erase(saved.origin);
}

TEST_F(ReleaseSetupTest, RepeatedConnectionFailureIsReportedAndServerSwitchResetsIt) {
  edition("operator");
  LauncherSettings settings;
  settings.server_id = "local";
  settings.game_directory = root_ / "runtime" / "game";
  settings.game_directory_explicit = true;
  SettingsStore(root_ / "out/launcher").save(settings);
  unsigned requests = 0;
  LauncherController controller(
      root_, root_, false,
      [&](const std::string &url) {
        ++requests;
        if (url.starts_with("http://127.0.0.1:")) {
          throw std::runtime_error("Synthetic offline local server");
        }
        return nlohmann::json{{"native_login_adapter", true}};
      },
      target_prefix_);

  controller.initialize();
  EXPECT_EQ(requests, 0);
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    controller.probe();
  }
  const auto failed = controller.state().at("server");
  EXPECT_FALSE(failed.at("running"));
  EXPECT_EQ(failed.at("connection_attempts"), 3);
  EXPECT_TRUE(failed.at("connection_error")
                  .get<std::string>()
                  .starts_with("Failed to connect to the server"));

  controller.select_server("live-na");
  EXPECT_EQ(requests, 3);
  EXPECT_EQ(controller.state().at("server").at("connection_error"), "");
  controller.probe();
  EXPECT_TRUE(controller.state().at("server").at("running"));
  EXPECT_EQ(controller.state().at("server").at("connection_attempts"), 0);
}

TEST_F(ReleaseSetupTest, LegacyRuntimeCleanupPreservesUnknownFilesAndBacksUpOnlyReceiptMatches) {
  const auto game = root_ / "game";
  std::filesystem::create_directories(game / "X5/Binaries/Win64");
  {
    std::ofstream marker(game / ".chronicles-install.json");
    marker << R"({"application":"chronicles"})";
    std::ofstream owned(game / "X5/Binaries/Win64/msvcp140.dll");
    owned << "owned runtime fixture";
    std::ofstream unrelated(game / "X5/Binaries/Win64/player-notes.txt");
    unrelated << "preserve";
  }
  const std::vector<distribution::PatchFile> receipts{
      {"X5/Binaries/Win64/msvcp140.dll", distribution::sha256_bytes("different bytes"), 21}};
  EXPECT_THROW(quarantine_managed_game_files(game, receipts), std::runtime_error);
  EXPECT_TRUE(std::filesystem::exists(game / receipts.front().path));

  auto owned_receipts = receipts;
  owned_receipts.front().sha256 = distribution::sha256_bytes("owned runtime fixture");
  const auto backup = quarantine_managed_game_files(game, owned_receipts);
  EXPECT_FALSE(backup.empty());
  EXPECT_EQ(backup.parent_path().parent_path(), root_);
  EXPECT_FALSE(std::filesystem::exists(game / receipts.front().path));
  EXPECT_TRUE(std::filesystem::exists(backup / receipts.front().path));
  EXPECT_TRUE(std::filesystem::exists(game / "X5/Binaries/Win64/player-notes.txt"));
  EXPECT_TRUE(quarantine_managed_game_files(game, owned_receipts).empty());
}

TEST(LegacyGameCompatibility, ExcludesOnlyTheExactPublishedLegacyRuntimeEntries) {
  distribution::Release release;
  release.version = "0.2.0";
  release.sequence = 2;
  release.payload_sha256 = std::string(64, 'a');
  release.files = {{"X5/Binaries/Win64/msvcp140.dll", std::string(64, 'b'), 100},
                   {"X5/Binaries/Win64/ArcheAgeChronicles.exe", std::string(64, 'c'), 500}};
  release.total_bytes = 600;
  const std::vector<distribution::PatchFile> receipts{release.files.front()};
  auto unexpected = release;
  unexpected.files.front().sha256 = std::string(64, 'd');
  EXPECT_THROW(exclude_legacy_game_runtimes(unexpected, receipts), std::runtime_error);

  exclude_legacy_game_runtimes(release, receipts);
  ASSERT_EQ(release.files.size(), 1);
  EXPECT_EQ(release.total_bytes, 500);
  EXPECT_EQ(release.payload_sha256, std::string(64, 'a'));
  EXPECT_EQ(release.files.front().path, "X5/Binaries/Win64/ArcheAgeChronicles.exe");
}

TEST(ManagedGameCompatibility, RejectsMissingSignedSteamRuntimeBeforeGameStart) {
  distribution::Release signed_release;
  signed_release.files = {
      {"Engine/Binaries/ThirdParty/Steamworks/Steamv161/Win64/steam_api64.dll",
       distribution::sha256_bytes("steam"), 5},
      {"Engine/Binaries/ThirdParty/Steamworks/Steamv161/Win64/libprotobuf.dll",
       distribution::sha256_bytes("protobuf"), 8}};

  EXPECT_THROW(
      require_managed_steam_compatibility(signed_release, std::filesystem::temp_directory_path()),
      std::runtime_error);
}

} // namespace
} // namespace chronicles::launcher
