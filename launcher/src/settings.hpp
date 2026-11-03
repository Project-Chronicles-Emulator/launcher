#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace chronicles::launcher {

struct LauncherSettings {
  std::filesystem::path game_directory;
  std::filesystem::path game_source_directory;
  bool game_directory_explicit{};
  bool allow_existing_game{};
  std::string server_id{"local"};
  bool release_catalog{};
  std::string steam_app_id{"5280930"};
  std::filesystem::path bridge_executable;
  std::filesystem::path openssl_executable{L"openssl.exe"};
  std::string backend_mode{"playsharp"};
  std::string region{"NA"};
  std::string account{"localdev"};
  std::string launch_mode{"named_account"};
  std::string client_launch_target{"steam"};
  std::string server_url;
  std::string patch_public_key;
  std::string patch_channel{"stable"};
  int http_port{8080};
  int world_port{8081};
  std::string world_host{"127.0.0.1"};
  // Agent engine (AI chat REST) port of a launcher-started server; 0 disables the listener. The
  // default matches the port of the client's hard-coded agent-engine URL.
  int agent_engine_port{38011};
  // Rewrites the client's agent-engine base URL to this server (docs/agent-engine.md).
  bool agent_engine_redirect{true};
  bool world_enabled{true};
  bool auto_start_server{false};
  bool auto_redirect{true};
  bool auto_world_entry{true};
  bool restore_on_exit{false};
  bool stop_server_on_exit{false};
};

// Settings live under ignored out/launcher/. They never contain passwords, bearer tokens or
// database connection strings.
class SettingsStore {
public:
  explicit SettingsStore(std::filesystem::path directory);

  LauncherSettings load() const;

  void save(const LauncherSettings &settings) const;

  const std::filesystem::path &directory() const;

  std::filesystem::path settings_path() const;

private:
  std::filesystem::path directory_;
};

nlohmann::json settings_to_json(const LauncherSettings &settings);

// Preserve an existing selection before considering automatic Steam discovery.
std::optional<std::filesystem::path> preferred_game_directory(
    const std::filesystem::path &selected_directory, bool selected_explicitly,
    const std::optional<std::filesystem::path> &steam_directory);

// Distribution profiles pin the API origin and signing key outside player-editable settings.
bool apply_distribution_profile(LauncherSettings &settings, const std::filesystem::path &directory);

void validate_distribution_settings(const LauncherSettings &settings);

} // namespace chronicles::launcher
