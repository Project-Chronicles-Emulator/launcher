#include "settings.hpp"

#include "distribution/distribution.hpp"
#include "encoding.hpp"
#include "game_discovery.hpp"
#include "game_process.hpp"
#include "release_servers.hpp"
#include "xorstr.hpp"

#include <algorithm>

#include <fstream>
#include <stdexcept>

namespace chronicles::launcher {
namespace {

void validate_port(int port, const char *name) {
  if (port < 1 || port > 65535) {
    throw std::invalid_argument(std::string(name) + " must be between 1 and 65535");
  }
}

// Checks shared by loading and saving, so a saved file always loads again.
void validate_settings(const LauncherSettings &settings) {
  validate_distribution_settings(settings);
  validate_port(settings.http_port, "http_port");
  validate_port(settings.world_port, "world_port");
  if (settings.world_enabled && settings.world_port == settings.http_port) {
    throw std::invalid_argument("HTTP and world ports must differ");
  }
  if (settings.agent_engine_port != 0) {
    validate_port(settings.agent_engine_port, "agent_engine_port");
    const bool same_as_http = settings.agent_engine_port == settings.http_port;
    const bool same_as_world =
        settings.world_enabled && settings.agent_engine_port == settings.world_port;
    if (same_as_http || same_as_world) {
      throw std::invalid_argument(
          "The agent engine port must differ from the HTTP and world ports");
    }
  }
  if (settings.launch_mode != "named_account" && settings.launch_mode != "disposable") {
    throw std::invalid_argument("Unknown launcher account mode");
  }
  if (settings.client_launch_target != "steam" && settings.client_launch_target != "private_copy" &&
      settings.client_launch_target != "managed") {
    throw std::invalid_argument("Unknown client launch target");
  }
}

std::string path_to_utf8(const std::filesystem::path &path) {
  return narrow(path.wstring());
}

std::filesystem::path read_path(const nlohmann::json &document, const char *key,
                                const std::filesystem::path &fallback) {
  const auto text = document.value(key, path_to_utf8(fallback));
  return std::filesystem::path(widen(text));
}

} // namespace

SettingsStore::SettingsStore(std::filesystem::path directory) : directory_(std::move(directory)) {}

LauncherSettings SettingsStore::load() const {
  LauncherSettings settings;
  const auto path = settings_path();
  if (!std::filesystem::is_regular_file(path)) {
    return settings;
  }

  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Could not read launcher settings");
  }

  const auto document = nlohmann::json::parse(input);
  settings.game_directory = read_path(document, "game_directory", settings.game_directory);
  settings.game_source_directory = read_path(document, "game_source_directory", {});
  settings.game_directory_explicit = document.value("game_directory_explicit", false);
  settings.server_id = document.value("server_id", settings.server_id);
  settings.steam_app_id = document.value("steam_app_id", settings.steam_app_id);
  settings.bridge_executable = read_path(document, "bridge_executable", settings.bridge_executable);
  settings.openssl_executable =
      read_path(document, "openssl_executable", settings.openssl_executable);
  settings.backend_mode = document.value("backend_mode", settings.backend_mode);
  settings.region = document.value("region", settings.region);
  settings.account = document.value("account", settings.account);
  settings.launch_mode = document.value("launch_mode", settings.launch_mode);
  settings.client_launch_target =
      document.value("client_launch_target", settings.client_launch_target);
  settings.server_url = document.value("server_url", settings.server_url);
  settings.patch_public_key = document.value("patch_public_key", settings.patch_public_key);
  settings.patch_channel = document.value("patch_channel", settings.patch_channel);
  settings.http_port = document.value("http_port", settings.http_port);
  settings.world_port = document.value("world_port", settings.world_port);
  settings.world_host = document.value("world_host", settings.world_host);
  settings.world_enabled = document.value("world_enabled", settings.world_enabled);
  settings.auto_start_server = document.value("auto_start_server", settings.auto_start_server);
  settings.auto_redirect = document.value("auto_redirect", settings.auto_redirect);
  settings.auto_world_entry = document.value("auto_world_entry", settings.auto_world_entry);
  settings.restore_on_exit = document.value("restore_on_exit", settings.restore_on_exit);
  settings.stop_server_on_exit =
      document.value("stop_server_on_exit", settings.stop_server_on_exit);
  settings.agent_engine_port = document.value("agent_engine_port", settings.agent_engine_port);
  settings.agent_engine_redirect =
      document.value("agent_engine_redirect", settings.agent_engine_redirect);

  validate_settings(settings);
  return settings;
}

void SettingsStore::save(const LauncherSettings &settings) const {
  validate_settings(settings);

  std::filesystem::create_directories(directory_);
  auto document = settings_to_json(settings);
  if (settings.client_launch_target == "managed") {
    // Credential Manager, not a launcher settings file, owns remembered account names.
    document.erase("account");
  }
  if (settings.release_catalog) {
    for (const auto *key : {"server_url", "patch_public_key", "world_host"}) {
      document.erase(key);
    }
  }
  const auto path = settings_path();
  const auto temporary_path = path.string() + ".tmp";

  {
    std::ofstream output(temporary_path, std::ios::trunc);
    if (!output) {
      throw std::runtime_error("Could not write launcher settings");
    }
    output << document.dump(2) << "\n";
  }

  std::error_code error;
  std::filesystem::rename(temporary_path, path, error);
  if (error) {
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary_path, path, error);
    if (error) {
      throw std::runtime_error("Could not replace launcher settings: " + error.message());
    }
  }
}

const std::filesystem::path &SettingsStore::directory() const {
  return directory_;
}

std::filesystem::path SettingsStore::settings_path() const {
  return directory_ / "settings.json";
}

nlohmann::json settings_to_json(const LauncherSettings &settings) {
  return {{"game_directory", path_to_utf8(settings.game_directory)},
          {"game_source_directory", path_to_utf8(settings.game_source_directory)},
          {"game_directory_explicit", settings.game_directory_explicit},
          {"server_id", settings.server_id},
          {"steam_app_id", settings.steam_app_id},
          {"bridge_executable", path_to_utf8(settings.bridge_executable)},
          {"openssl_executable", path_to_utf8(settings.openssl_executable)},
          {"backend_mode", settings.backend_mode},
          {"region", settings.region},
          {"account", settings.account},
          {"launch_mode", settings.launch_mode},
          {"client_launch_target", settings.client_launch_target},
          {"server_url", settings.server_url},
          {"patch_public_key", settings.patch_public_key},
          {"patch_channel", settings.patch_channel},
          {"http_port", settings.http_port},
          {"world_port", settings.world_port},
          {"world_host", settings.world_host},
          {"world_enabled", settings.world_enabled},
          {"auto_start_server", settings.auto_start_server},
          {"auto_redirect", settings.auto_redirect},
          {"auto_world_entry", settings.auto_world_entry},
          {"restore_on_exit", settings.restore_on_exit},
          {"stop_server_on_exit", settings.stop_server_on_exit},
          {"agent_engine_port", settings.agent_engine_port},
          {"agent_engine_redirect", settings.agent_engine_redirect}};
}

void validate_distribution_settings(const LauncherSettings &settings) {
  if (!settings.server_url.empty()) {
    const auto origin = distribution::parse_service_origin(settings.server_url);
    if (!origin.loopback && settings.launch_mode != "named_account") {
      throw std::invalid_argument("Remote servers require named-account login");
    }
  }
  if ((!settings.patch_public_key.empty() &&
       !distribution::valid_sha256(settings.patch_public_key)) ||
      !distribution::valid_channel(settings.patch_channel)) {
    throw std::invalid_argument("Invalid patch public key or channel");
  }
}

std::optional<std::filesystem::path> preferred_game_directory(
    const std::filesystem::path &selected_directory, bool selected_explicitly,
    const std::optional<std::filesystem::path> &steam_directory) {
  if (selected_explicitly) {
    return selected_directory;
  }

  std::error_code file_error;
  const bool selected_game_exists =
      !selected_directory.empty() &&
      std::filesystem::is_regular_file(expected_client_executable(selected_directory), file_error);
  if (selected_game_exists) {
    return selected_directory;
  }

  return steam_directory;
}

bool apply_distribution_profile(LauncherSettings &settings,
                                const std::filesystem::path &directory) {
  const auto path = directory / "launcher-config.json";
  if (!std::filesystem::is_regular_file(path)) {
    return false;
  }
  auto configured = settings;
  const auto document = nlohmann::json::parse(distribution::read_bounded_file(path, 65536));
  const auto servers = release_servers(directory);
  configured.release_catalog = !servers.empty();
  if (!servers.empty()) {
    auto selected = std::ranges::find(servers, configured.server_id, &ReleaseServer::id);
    if (selected == servers.end()) {
      selected = servers.begin();
    }
    configured.server_id = selected->id;
    configured.server_url = selected->origin;
    configured.patch_public_key = selected->patch_public_key;
    configured.patch_channel = selected->channel;
    configured.world_port = selected->world_port;
    configured.world_host = selected->world_host;
  } else {
    configured.server_url =
        distribution::parse_service_origin(document.at("server_url").get<std::string>()).url;
    configured.patch_public_key = document.at("patch_public_key").get<std::string>();
    configured.patch_channel = document.value("patch_channel", std::string("stable"));
    if (configured.patch_public_key.empty()) {
      throw std::invalid_argument("Distribution profile requires a pinned patch public key");
    }
  }
  configured.launch_mode = "named_account";
  const bool unsigned_local =
      distribution::parse_service_origin(configured.server_url).loopback &&
      configured.patch_public_key.empty();
  if (unsigned_local) {
    configured.http_port = distribution::parse_service_origin(configured.server_url).port;
  }
  if (!unsigned_local) {
    configured.client_launch_target = "managed";
  } else if (configured.client_launch_target == "managed") {
    configured.client_launch_target = "steam";
  }
  configured.backend_mode = "usermode";
  configured.bridge_executable.clear();
  if (configured.account == "localdev") {
    configured.account.clear();
  }
  configured.auto_start_server = false;
  configured.auto_redirect = unsigned_local;
  configured.auto_world_entry = !servers.empty();
  configured.restore_on_exit = false;
  configured.stop_server_on_exit = false;
  // A hosted server's agent engine is loopback-only by default; it has no client credential.
  configured.agent_engine_redirect = false;
  std::optional<std::filesystem::path> steam_game;
  try {
    steam_game = discover_existing_game();
  } catch (const std::exception &) {
    // Invalid Steam metadata must not prevent using a separately managed installation.
  }

  const auto preferred_game = preferred_game_directory(
      configured.game_directory, configured.game_directory_explicit, steam_game);
  if (preferred_game) {
    configured.game_directory = *preferred_game;
  } else {
    const auto existing_game = discover_other_game(directory, configured.patch_channel);
    configured.game_directory = existing_game.value_or(directory / "runtime" / "game");
  }

  std::error_code file_error;
  configured.allow_existing_game = !configured.game_directory.empty() &&
      std::filesystem::is_regular_file(expected_client_executable(configured.game_directory),
                                       file_error);
  if (configured.game_source_directory.empty() && steam_game) {
    configured.game_source_directory = *steam_game;
  }
  validate_distribution_settings(configured);
  settings = std::move(configured);
  return true;
}

} // namespace chronicles::launcher
