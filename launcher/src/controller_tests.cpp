#include "bootstrap_archive.hpp"
#include "controller.hpp"

#include <windows.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace chronicles::launcher {
namespace {

class LauncherAccountTest : public testing::Test {
protected:
  void SetUp() override {
    runtime_root_ = std::filesystem::temp_directory_path() /
                    ("chronicles-launcher-account-test-" + std::to_string(GetCurrentProcessId()) +
                     "-" + std::to_string(GetTickCount64()));
    ASSERT_TRUE(std::filesystem::create_directory(runtime_root_));
    target_prefix_ = L"Project Chronicles Controller Test/" + runtime_root_.filename().wstring() + L"/";
  }

  void TearDown() override {
    const auto resolved_root = std::filesystem::canonical(runtime_root_);
    ASSERT_EQ(resolved_root.parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    ASSERT_TRUE(resolved_root.filename().string().starts_with("chronicles-launcher-account-test-"));
    std::error_code cleanup_error;
    std::filesystem::remove_all(resolved_root, cleanup_error);
    EXPECT_FALSE(cleanup_error) << cleanup_error.message();
  }

  std::filesystem::path runtime_root_;
  std::wstring target_prefix_;
};

TEST_F(LauncherAccountTest, InvalidAccountLeavesSelectionUnchangedAndPublishesFailedAttempt) {
  LauncherController controller(runtime_root_, runtime_root_ / "launcher", false, {}, target_prefix_);
  const auto original_account = controller.state().at("settings").at("account");

  EXPECT_THROW(controller.launch_game("transient-test-password", "xx"), std::invalid_argument);
  const auto first_failure = controller.state();
  EXPECT_EQ(first_failure.at("settings").at("account"), original_account);
  EXPECT_EQ(first_failure.at("launch").at("status"), "failed");
  EXPECT_EQ(first_failure.at("launch").at("attempt"), 1);
  EXPECT_EQ(first_failure.dump().find("transient-test-password"), std::string::npos);

  EXPECT_THROW(controller.launch_game("transient-test-password", "xx"), std::invalid_argument);
  EXPECT_EQ(controller.state().at("launch").at("attempt"), 2);
  EXPECT_FALSE(std::filesystem::exists(runtime_root_ / "out" / "launcher" / "settings.json"));
}

TEST_F(LauncherAccountTest, SettingsWriteFailureStopsBeforeLaunchingPreviouslySelectedAccount) {
  // A file blocks creation of out/launcher. This fails before contacting any login service.
  {
    std::ofstream blocked_directory(runtime_root_ / "out");
    ASSERT_TRUE(blocked_directory.good());
    blocked_directory << "Block settings directory creation";
  }

  LauncherController controller(runtime_root_, runtime_root_ / "launcher", false, {}, target_prefix_);
  const auto original_account = controller.state().at("settings").at("account");

  EXPECT_THROW(controller.launch_game("another-transient-password", "other-account"),
               std::filesystem::filesystem_error);
  const auto failure = controller.state();
  EXPECT_EQ(failure.at("settings").at("account"), original_account);
  EXPECT_EQ(failure.at("launch").at("status"), "failed");
  EXPECT_EQ(failure.dump().find("another-transient-password"), std::string::npos);
  EXPECT_TRUE(failure.at("activity").empty());
}

TEST_F(LauncherAccountTest, DistributionProfilePinsOriginAndDisablesLocalHelpers) {
  {
    std::ofstream profile(runtime_root_ / "launcher-config.json");
    profile << nlohmann::json{{"server_url", "https://realm.example"},
                              {"patch_public_key", std::string(64, '1')},
                              {"patch_channel", "stable"}}
                   .dump();
  }
  LauncherSettings settings;
  settings.game_directory = runtime_root_ / "runtime" / "game";
  settings.game_directory_explicit = true;
  ASSERT_TRUE(apply_distribution_profile(settings, runtime_root_));
  EXPECT_EQ(settings.server_url, "https://realm.example");
  EXPECT_EQ(settings.patch_public_key, std::string(64, '1'));
  EXPECT_FALSE(settings.auto_world_entry);
  EXPECT_FALSE(settings.auto_redirect);
  EXPECT_FALSE(settings.auto_start_server);
  EXPECT_EQ(settings.launch_mode, "named_account");
  EXPECT_EQ(settings.client_launch_target, "managed");
  EXPECT_EQ(settings.backend_mode, "usermode");
  EXPECT_TRUE(settings.bridge_executable.empty());
  EXPECT_TRUE(settings.account.empty());
  EXPECT_EQ(settings.game_directory, runtime_root_ / "runtime" / "game");
}

TEST_F(LauncherAccountTest, SignOutDeletionFailureClearsBusyState) {
  const auto origin = std::string("http://127.0.0.1:8080");
  const auto legacy = runtime_root_ / "out/launcher/login.dat";
  CredentialStore store(legacy, target_prefix_);
  SavedLogin saved;
  saved.origin = origin;
  saved.account = "alice";
  saved.token = std::string(64, 'a');
  saved.keep_signed_in = true;
  store.save(saved);

  LauncherController controller(runtime_root_, runtime_root_ / "launcher", false, {}, target_prefix_);
  controller.initialize();
  ASSERT_TRUE(controller.state().at("auth").at("authenticated"));
  std::filesystem::create_directories(legacy.parent_path());
  HANDLE locked = CreateFileW(legacy.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(locked, INVALID_HANDLE_VALUE);
  EXPECT_THROW(controller.sign_out(), std::runtime_error);
  EXPECT_NE(controller.state().at("auth").at("status"), "signing_in");
  CloseHandle(locked);
  store.erase(origin);
}

TEST_F(LauncherAccountTest, RealmChangeToDisposableKeepsPreviousSavedCredential) {
  const auto origin = std::string("http://127.0.0.1:8080");
  const auto legacy = runtime_root_ / "out/launcher/login.dat";
  CredentialStore store(legacy, target_prefix_);
  SavedLogin saved;
  saved.origin = origin;
  saved.account = "alice";
  saved.token = std::string(64, 'a');
  saved.keep_signed_in = true;
  store.save(saved);

  LauncherController controller(runtime_root_, runtime_root_ / "launcher", false, {}, target_prefix_);
  controller.initialize();
  ASSERT_TRUE(controller.state().at("auth").at("authenticated"));
  controller.save_settings({{"server_url", "http://127.0.0.1:8082"},
                            {"launch_mode", "disposable"}});
  EXPECT_FALSE(controller.state().at("auth").at("authenticated"));
  controller.sign_out();
  ASSERT_NE(store.load(origin, {}), nullptr);
  EXPECT_EQ(store.load(origin, {})->account, "alice");
  store.erase(origin);
}

TEST_F(LauncherAccountTest, ManagedSettingsDoNotPersistTheAccountName) {
  LauncherSettings settings;
  settings.client_launch_target = "managed";
  settings.account = "remembered_account";
  SettingsStore store(runtime_root_ / "settings");
  store.save(settings);

  const auto document = nlohmann::json::parse(
      distribution::read_bounded_file(store.settings_path(), 65536));
  EXPECT_FALSE(document.contains("account"));
}

TEST_F(LauncherAccountTest, PackagedSettingsAcceptOnlyTheGameFolder) {
  {
    std::ofstream profile(runtime_root_ / "launcher-config.json");
    profile << nlohmann::json{{"server_url", "http://127.0.0.1:1"},
                              {"patch_public_key", std::string(64, '1')},
                              {"patch_channel", "stable"}}
                   .dump();
  }

  LauncherController controller(runtime_root_, runtime_root_, false, {}, target_prefix_);
  controller.probe();

  EXPECT_THROW(controller.save_player_settings({{"backend_mode", "playsharp"}}),
               std::invalid_argument);
  EXPECT_THROW(controller.save_player_settings({{"game_directory", "chosen-game"},
                                                {"account", "other-account"}}),
               std::invalid_argument);

  const auto selected_game = runtime_root_ / "chosen-game";
  controller.save_player_settings({{"game_directory", selected_game.string()}});
  EXPECT_EQ(controller.state().at("settings").at("game_directory"), selected_game.string());
}

TEST_F(LauncherAccountTest, PlayerInterfaceKeepsLocalServiceSettings) {
  {
    std::ofstream marker(runtime_root_ / "launcher-player-ui.flag");
    marker << "Player interface for the local launcher\n";
  }

  LauncherSettings saved_settings;
  saved_settings.backend_mode = "playsharp";
  saved_settings.client_launch_target = "private_copy";
  SettingsStore(runtime_root_ / "out" / "launcher").save(saved_settings);

  LauncherController controller(runtime_root_, runtime_root_, false, {}, target_prefix_);
  controller.probe();
  const auto initial_state = controller.state();
  EXPECT_TRUE(initial_state.at("player_interface").get<bool>());
  EXPECT_FALSE(initial_state.at("distribution_profile").get<bool>());
  EXPECT_EQ(initial_state.at("settings").at("backend_mode"), "playsharp");
  EXPECT_EQ(initial_state.at("settings").at("client_launch_target"), "private_copy");

  EXPECT_THROW(controller.save_player_settings({{"backend_mode", "usermode"}}),
               std::invalid_argument);

  const auto selected_game = runtime_root_ / "private-game";
  controller.save_player_settings({{"game_directory", selected_game.string()}});
  const auto updated_state = controller.state();
  EXPECT_EQ(updated_state.at("settings").at("game_directory"), selected_game.string());
  EXPECT_EQ(updated_state.at("settings").at("backend_mode"), "playsharp");
}

TEST_F(LauncherAccountTest, InvalidDistributionProfileLeavesPreviousSettingsIntact) {
  LauncherSettings settings;
  const auto before = settings_to_json(settings);
  {
    std::ofstream profile(runtime_root_ / "launcher-config.json");
    profile
        << nlohmann::json{{"server_url", "https://realm.example"}, {"patch_public_key", ""}}.dump();
  }
  EXPECT_THROW(apply_distribution_profile(settings, runtime_root_), std::invalid_argument);
  EXPECT_EQ(settings_to_json(settings), before);
}

TEST_F(LauncherAccountTest,
       RememberingAnAccountDoesNotAuthenticateAndFailedLoginKeepsThatSelection) {
  LauncherController controller(runtime_root_, runtime_root_ / "launcher", false, {}, target_prefix_);
  controller.save_settings({{"account", "remembered_account"}});
  EXPECT_FALSE(controller.state().at("auth").at("authenticated"));
  EXPECT_EQ(controller.state().at("auth").at("account"), "");
  EXPECT_THROW(controller.sign_in("do-not-persist-this-password", "bad/account"),
               std::invalid_argument);
  EXPECT_EQ(controller.state().at("auth").at("status"), "error");
  EXPECT_EQ(controller.state().at("settings").at("account"), "remembered_account");
  EXPECT_EQ(controller.state().dump().find("do-not-persist-this-password"), std::string::npos);
  const auto saved =
      distribution::read_bounded_file(runtime_root_ / "out/launcher/settings.json", 65536);
  EXPECT_EQ(saved.find("do-not-persist-this-password"), std::string::npos);
}

TEST_F(LauncherAccountTest, SignedBootstrapUsesOnlyTheVerifiedFixedLengthGrantPlaceholder) {
  const std::string origin = "http://127.0.0.1:8480";
  const auto placeholder = origin + "/launch/" + std::string(64, '0') + "/";
  const auto grant_url = origin + "/launch/" + std::string(64, 'b') + "/";
  std::string template_bytes(4, '\0');
  const auto length = static_cast<std::uint32_t>(placeholder.size() + 1);
  for (unsigned index = 0; index < 4; ++index) {
    template_bytes[index] = static_cast<char>((length >> (index * 8)) & 255);
  }
  template_bytes += placeholder;
  template_bytes.push_back('\0');
  template_bytes += "opaque template suffix";
  distribution::Release release;
  release.api_origin = origin;
  release.bootstrap = distribution::PatchFile{"", distribution::sha256_bytes(template_bytes),
                                              template_bytes.size()};
  const auto cached = runtime_root_ / ("bootstrap-template-" + release.bootstrap->sha256 + ".bin");
  {
    std::ofstream output(cached, std::ios::binary);
    output.write(template_bytes.data(), static_cast<std::streamsize>(template_bytes.size()));
  }
  const auto output = prepare_signed_bootstrap(release, runtime_root_, grant_url, {});
  const auto prepared = distribution::read_bounded_file(output, 65536);
  EXPECT_EQ(prepared.size(), template_bytes.size());
  EXPECT_NE(prepared.find(grant_url), std::string::npos);
  EXPECT_EQ(prepared.find(placeholder), std::string::npos);
  EXPECT_TRUE(prepared.ends_with("opaque template suffix"));
}

} // namespace
} // namespace chronicles::launcher
