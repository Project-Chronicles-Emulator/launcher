#include "controller.hpp"
#include "world_connection.hpp"

#include "bootstrap_archive.hpp"
#include "encoding.hpp"
#include "game_compatibility.hpp"
#include "http_client.hpp"
#include "prerequisites.hpp"
#include "release_servers.hpp"
#include "xorstr.hpp"

#include "usermode_process_memory.hpp"

#include <windows.h>

#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace chronicles::launcher {
namespace {

using json = nlohmann::json;

constexpr int kHealthTimeoutMs = 700;

// Developer checkouts build every executable into CMake's shared Release output folder
// (CMakeLists.txt sets CMAKE_RUNTIME_OUTPUT_DIRECTORY).
std::filesystem::path development_release_directory(const std::filesystem::path &project_root) {
  return project_root / "out" / "build" / "windows" / "bin" / "Release";
}

struct SecretClear {
  std::string &value;

  ~SecretClear() {
    if (!value.empty()) {
      SecureZeroMemory(value.data(), value.size());
    }
  }
};

std::string timestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  localtime_s(&local, &time);

  char buffer[16];
  std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
  return buffer;
}

json target_to_json(const RedirectTarget &target) {
  return {{"role", target.role},
          {"header", format_address(target.header_address)},
          {"buffer", format_address(target.buffer_address)},
          {"header_base64", base64_encode(target.original_header_bytes)},
          {"original_base64", base64_encode(target.original_text_bytes)},
          {"original_address", target.original_address},
          {"replacement_address", target.replacement_address}};
}

RedirectTarget target_from_json(const json &document) {
  RedirectTarget target;
  target.role = document.at("role").get<std::string>();
  target.header_address = parse_address(document.at("header").get<std::string>());
  target.buffer_address = parse_address(document.at("buffer").get<std::string>());
  target.original_header_bytes = base64_decode(document.at("header_base64").get<std::string>());
  target.original_text_bytes = base64_decode(document.at("original_base64").get<std::string>());
  target.original_address = document.value("original_address", std::string{});
  target.replacement_address = document.value("replacement_address", std::string{});
  return target;
}

} // namespace

LauncherController::LauncherController(std::filesystem::path project_root,
                                       std::filesystem::path launcher_directory,
                                       bool live_servers_only, HealthRequest health_request,
                                       std::wstring credential_target_prefix)
    : project_root_(std::move(project_root)), launcher_directory_(std::move(launcher_directory)),
      settings_store_(project_root_ / "out" / "launcher"),
      account_session_({}, {}, settings_store_.directory() / "login.dat",
                       std::move(credential_target_prefix)),
      health_request_(std::move(health_request)), live_servers_only_(live_servers_only) {
  if (!health_request_) {
    health_request_ = [](const std::string &url) { return http_get_json(url, kHealthTimeoutMs); };
  }
  player_interface_ =
      std::filesystem::is_regular_file(launcher_directory_ / "launcher-player-ui.flag");
}

void LauncherController::set_state_sink(std::function<void(const std::string &)> sink) {
  state_sink_ = std::move(sink);
}

void LauncherController::publish_state() {
  emit_state();
}

void LauncherController::initialize() {
  try {
    settings_ = settings_store_.load();
  } catch (const std::exception &error) {
    last_error_ = std::string("Settings were reset: ") + error.what();
    settings_ = LauncherSettings{};
  }

  try {
    distribution_profile_ = apply_distribution_profile(settings_, launcher_directory_);
    if (live_servers_only_ && distribution::parse_service_origin(service_url()).loopback) {
      throw std::invalid_argument("The player launcher connects to live Project Chronicles servers only.");
    }
  } catch (...) {
    configuration_failed_ = true;
    throw;
  }
  initialized_ = true;
  if (settings_.launch_mode == "named_account") {
    account_session_.restore_saved_login(service_url(), settings_.account, false);
  }
  patch_check_requested_ = !settings_.patch_public_key.empty();
  emit_state();
  const auto server_executable =
      development_release_directory(project_root_) / "chronicles-server.exe";
  log("Launcher ready. Server executable: " + server_executable.string());
  if (!last_error_.empty()) {
    log(last_error_);
  }

  refresh_client_status();
  if (settings_.launch_mode == "disposable" && settings_.auto_start_server) {
    refresh_server_status();
    try {
      if (!server_running_) {
        start_server();
      }
    } catch (const std::exception &error) {
      last_error_ = error.what();
      log(std::string("Automatic server start failed: ") + error.what());
    }
  }
  emit_state();
}

void LauncherController::probe() {
  settings_ = settings_store_.load();
  distribution_profile_ = apply_distribution_profile(settings_, launcher_directory_);
  if (live_servers_only_ && distribution::parse_service_origin(service_url()).loopback) {
    throw std::invalid_argument("The player launcher connects to live Project Chronicles servers only.");
  }
  initialized_ = true;
  refresh_server_status();
  refresh_client_status();
}

void LauncherController::refresh() {
  if (!initialized_ || shutting_down_ || stop_requested()) {
    return;
  }

  refresh_server_status();
  const auto previous_client_pids = client_pids_;
  refresh_client_status();
  if (client_pids_ != previous_client_pids) {
    emit_state();
  }
  refresh_world_assist();
  account_session_.tick_discord_login(server_running_);
  const auto signed_in_account = account_session_.state().at("account").get<std::string>();
  if (!signed_in_account.empty() && signed_in_account != settings_.account &&
      account_session_.matches(service_url(), signed_in_account)) {
    auto updated = settings_;
    updated.account = signed_in_account;
    try {
      settings_store_.save(updated);
      settings_ = std::move(updated);
    } catch (...) {
      account_session_.sign_out();
      throw;
    }
  }
  // An offline realm must not occupy the action worker with additional requests.
  // Its catalog and selection remain available while only health is retried.
  if (server_running_) {
    account_session_.refresh_discord_options(service_url());
    news_feed_.refresh(service_url());
    account_session_.refresh();
    account_session_.refresh_characters();
    portal_feed_.refresh(service_url());
  }
  refresh_redirect();
  refresh_agent_engine_redirect();
  if (server_running_ && patch_check_requested_ && !settings_.patch_public_key.empty()) {
    patch_check_requested_ = false;
    try {
      if (settings_.game_directory.empty()) {
        check_updates();
      } else {
        update_game();
      }
    } catch (const std::exception &error) {
      note_error(error.what());
    }
  }

  if (++ticks_since_log_read_ >= 4) {
    ticks_since_log_read_ = 0;
    server_log_tail_ = server_.log_tail(6000);
    std::string combined_world_logs;
    for (const auto &[client_pid, session] : world_assists_) {
      const auto client_log = session.process->log_tail(2000);
      if (!client_log.empty()) {
        combined_world_logs += "Client " + std::to_string(client_pid) + ":\n" + client_log;
      }
    }
    if (!combined_world_logs.empty()) {
      constexpr std::size_t kMaximumWorldLogCharacters = 6000;
      if (combined_world_logs.size() > kMaximumWorldLogCharacters) {
        combined_world_logs.erase(0, combined_world_logs.size() - kMaximumWorldLogCharacters);
      }
      world_assist_log_tail_ = std::move(combined_world_logs);
    }
  }
  emit_state();
}

void LauncherController::refresh_news() {
  auto loading = state();
  loading["news"]["status"] = "loading";
  if (state_sink_) {
    state_sink_(loading.dump());
  }
  news_feed_.refresh(service_url(), true);
  emit_state();
}

void LauncherController::refresh_content() {
  auto loading = state();
  loading["portal"]["status"] = "loading";
  if (state_sink_) {
    state_sink_(loading.dump());
  }
  portal_feed_.refresh(service_url(), true);
  emit_state();
}

void LauncherController::refresh_characters() {
  auto loading = state();
  if (account_session_.state().at("authenticated").get<bool>()) {
    loading["auth"]["characters"]["status"] = "loading";
  }
  if (state_sink_) {
    state_sink_(loading.dump());
  }
  account_session_.refresh_characters(true);
  emit_state();
}

std::string LauncherController::portal_link(const std::string &kind, const std::string &id) const {
  return portal_feed_.link(kind, id);
}

void LauncherController::start_server() {
  if (settings_.launch_mode != "disposable") {
    throw std::runtime_error("Select disposable mode to start a launcher-managed server");
  }
  if (server_.is_running()) {
    throw std::runtime_error("The launcher already started a server");
  }

  server_.start(server_options());
  server_pid_ = server_.pid();
  log("Started chronicles-server (PID " + std::to_string(server_pid_) +
      ") on 127.0.0.1:" + std::to_string(settings_.http_port) +
      (settings_.world_enabled
           ? " with the prototype TLS world listener on " + std::to_string(settings_.world_port)
           : std::string{}));

  // The server initializes its password hash before listening; give it a short readiness window.
  for (int attempt = 0; attempt < 40; ++attempt) {
    if (!server_.is_running()) {
      throw std::runtime_error("The server exited during startup; inspect the Server log tab");
    }
    try {
      health_ = http_get_json(std::string(xorstr_("http://127.0.0.1:")) +
                                  std::to_string(settings_.http_port) + "/health",
                              kHealthTimeoutMs);
      if (health_.value("native_login_adapter", false)) {
        server_running_ = true;
        external_server_ = false;
        log("Local native auth/lobby is ready");
        emit_state();
        return;
      }
    } catch (const std::exception &) {
      // Expected while the listener is still starting.
    }
    Sleep(250);
  }

  throw std::runtime_error("The server did not become ready; inspect the Server log tab");
}

void LauncherController::stop_server() {
  if (!server_.is_running()) {
    if (external_server_) {
      throw std::runtime_error(
          "This server was not started by the launcher; stop its own process instead");
    }
    return;
  }

  server_.stop();
  server_running_ = false;
  server_pid_ = 0;
  health_ = json::object();
  log("Stopped the launcher-managed server");
  emit_state();
}

void LauncherController::launch_game(std::string password, const std::string &account) {
  SecretClear clear_password{password};
  if (configuration_failed_) {
    throw std::runtime_error("Repair the invalid distribution profile before launching the game");
  }
  ++launch_attempt_;
  launch_status_ = "preparing";
  launch_error_.clear();
  last_error_.clear();
  emit_state();

  try {
    // Account selection and launch are one queued operation. A failed settings write must
    // never launch the previously selected account with a different account's password.
    if (!account.empty() && account != settings_.account) {
      save_settings({{"account", account}});
    }
    launch_game_client(password);
    launch_status_ = "started";
    launch_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(90);
  } catch (const std::exception &error) {
    launch_status_ = "failed";
    launch_error_ = error.what();
    emit_state();
    throw;
  }
  emit_state();
}

void LauncherController::sign_in(std::string password, const std::string &account,
                                 bool keep_signed_in, bool remember_password,
                                 bool use_saved_password) {
  SecretClear clear_password{password};
  if (configuration_failed_ || settings_.launch_mode != "named_account") {
    throw std::runtime_error("Select a valid named-account configuration before signing in.");
  }
  refresh_client_status();
  if (client_ && settings_.client_launch_target != "private_copy") {
    throw std::runtime_error("Close the game before signing in to another account.");
  }
  account_busy_ = true;
  last_error_.clear();
  emit_state();
  try {
    account_session_.sign_in(service_url(), account, password, keep_signed_in, remember_password,
                             use_saved_password);
    auto updated = settings_;
    updated.account = account_session_.state().at("account").get<std::string>();
    try {
      settings_store_.save(updated);
    } catch (...) {
      account_session_.sign_out();
      throw;
    }
    settings_ = std::move(updated);
    log("Account signed in for this launcher session");
    account_busy_ = false;
    emit_state();
  } catch (...) {
    account_busy_ = false;
    emit_state();
    throw;
  }
}

void LauncherController::set_login_preferences(bool keep_signed_in, bool remember_password) {
  account_session_.set_login_preferences(keep_signed_in, remember_password);
  emit_state();
}

std::string LauncherController::begin_discord_login(bool keep_signed_in) {
  refresh_client_status();
  if (configuration_failed_ || settings_.launch_mode != "named_account" || client_ || patch_active_) {
    throw std::runtime_error("Close the game and finish its update before Discord sign-in.");
  }
  const auto url = account_session_.begin_discord_login(service_url(), keep_signed_in);
  emit_state();
  return url;
}

void LauncherController::cancel_discord_login() {
  account_session_.cancel_discord_login();
  emit_state();
}

void LauncherController::forget_saved_login() {
  account_session_.forget_saved_login();
  emit_state();
}

std::string LauncherController::register_account(std::string password, const std::string &account) {
  SecretClear clear_password{password};
  if (configuration_failed_ || settings_.launch_mode != "named_account") {
    throw std::runtime_error("Select a valid named-account realm before creating an account.");
  }
  const auto created_account = account_session_.register_account(service_url(), account, password);
  last_error_.clear();
  log("Player account created. Sign in to begin.");
  emit_state();
  return created_account;
}

void LauncherController::sign_out() {
  account_busy_ = true;
  emit_state();
  try {
    account_session_.sign_out();
    account_busy_ = false;
    last_error_.clear();
    log("Signed out of the launcher. Running game processes were left open.");
    emit_state();
  } catch (...) {
    account_busy_ = false;
    emit_state();
    throw;
  }
}

std::string LauncherController::service_url() const {
  if (settings_.server_url.empty()) {
    return std::string(xorstr_("http://127.0.0.1:")) + std::to_string(settings_.http_port);
  }
  return distribution::parse_service_origin(settings_.server_url).url;
}

void LauncherController::check_updates(std::stop_token stop, bool verify_files,
                                       bool inspect_files) {
  // Manual checks and updates consume the scheduled startup check as well. A
  // cancellation must not be followed by a second automatic update.
  patch_check_requested_ = false;
  try {
    if (stop.stop_requested()) {
      throw std::runtime_error("Update cancelled");
    }
    if (configuration_failed_) {
      throw std::runtime_error("Repair the invalid distribution profile before updating the game");
    }
    if (settings_.patch_public_key.empty()) {
      throw std::runtime_error(
          "Configure the patch signing public key before checking for updates");
    }
    if (!find_client_processes(settings_.game_directory).empty()) {
      throw std::runtime_error("Close the game before checking or recovering an installation");
    }
    patch_state_["status"] = "checking";
    patch_state_["verification"] = "release_catalog";
    patch_state_["error"] = "";
    emit_state();
    const auto envelope = http_get_text(service_url() + xorstr_("/distribution/v1/channels/") +
                                            settings_.patch_channel,
                                        3000, stop);
    auto release = distribution::verify_release(envelope, settings_.patch_public_key,
                                                settings_.patch_channel, service_url());
    if (release.version == "0.2.0" && release.sequence == 2) {
      exclude_legacy_game_runtimes(release, legacy_game_runtime_files(launcher_directory_));
    }
    if (stop.stop_requested()) {
      throw std::runtime_error("Update cancelled");
    }
    patch_state_["version"] = release.version;
    patch_state_["sequence"] = release.sequence;
    patch_release_ = std::move(release);
    if (settings_.game_directory.empty()) {
      patch_state_["status"] = "choose_directory";
    } else {
      const auto existing_directory = settings_.allow_existing_game
                                          ? ExistingDirectory::game
                                          : ExistingDirectory::managed_only;
      PatchInstaller installer(settings_.game_directory, existing_directory);
      if (!verify_files && installer.is_current_release(*patch_release_)) {
        installer.remember_current_release(*patch_release_);
        patch_state_["status"] = "ready";
        patch_state_["verification"] = "installed_metadata";
        patch_state_["total_bytes"] = 0;
        patch_state_["completed_bytes"] = 0;
        patch_state_["files"] = 0;
        patch_state_["error"] = "";
        emit_state();
        return;
      }
      if (!inspect_files) {
        // install() acquires the installation lock and verifies the files once. A separate
        // preflight scan would read a large existing archive twice before any repair begins.
        patch_state_["verification"] = "pending_install";
        patch_state_["total_bytes"] = 0;
        patch_state_["completed_bytes"] = 0;
        patch_state_["files"] = 0;
        patch_state_["current_file"] = "";
        patch_state_["status"] = "available";
        emit_state();
        return;
      }
      patch_state_["verification"] = "full";
      const auto plan =
          installer.inspect(*patch_release_, stop,
                            [&](auto completed, auto total, const auto &path, const auto &phase) {
                              patch_state_["completed_bytes"] = completed;
                              patch_state_["total_bytes"] = total;
                              patch_state_["current_file"] = path;
                              patch_state_["status"] = phase;
                              emit_state();
                            });
      patch_state_["total_bytes"] = plan.download_bytes;
      patch_state_["completed_bytes"] = 0;
      patch_state_["files"] = plan.changed.size();
      patch_state_["status"] = plan.changed.empty() ? "ready" : "available";
    }
    emit_state();
  } catch (const std::exception &error) {
    patch_state_["status"] = "error";
    patch_state_["error"] = error.what();
    emit_state();
    throw;
  }
}

void LauncherController::cancel_update() {
  std::scoped_lock lock(patch_stop_mutex_);
  patch_stop_.request_stop();
}

void LauncherController::update_game(bool verify_files) {
  verify_files = verify_files || patch_state_.value("status", std::string{}) == "available";
  std::stop_token patch_token;
  {
    std::scoped_lock lock(patch_stop_mutex_);
    patch_stop_ = std::stop_source{};
    if (stop_requested()) {
      patch_stop_.request_stop();
    }
    patch_token = patch_stop_.get_token();
  }
  patch_active_ = true;
  patch_state_["warning"] = "";
  try {
    check_updates(patch_token, verify_files, false);
    if (patch_state_.value("verification", std::string{}) == "installed_metadata") {
      patch_active_ = false;
      log("Installed game release " + patch_release_->version + " is current");
      emit_state();
      return;
    }
    if (settings_.game_directory.empty()) {
      throw std::runtime_error("Choose an empty game installation directory in Settings");
    }
    const auto existing_directory = settings_.allow_existing_game
                                        ? ExistingDirectory::game
                                        : ExistingDirectory::managed_only;
    PatchInstaller installer(settings_.game_directory, existing_directory);
    auto last_progress = std::chrono::steady_clock::time_point{};
    std::string last_phase;
    installer.install(
        *patch_release_,
        [&](const auto &file, const auto &destination, const auto &progress, auto stop) {
          if (!settings_.game_source_directory.empty()) {
            const auto source = settings_.game_source_directory / widen(file.path);
            require_regular_patch_path(source);
            if (std::filesystem::is_regular_file(source) &&
                std::filesystem::file_size(source) == file.size &&
                distribution::sha256_file(source, stop) == file.sha256) {
              std::filesystem::copy_file(source, destination,
                                         std::filesystem::copy_options::overwrite_existing);
              progress(file.size);
              return;
            }
          }
          http_download_file(service_url() + std::string(xorstr_("/distribution/v1/blobs/")) +
                                 file.sha256,
                             destination, file.size, progress, stop);
        },
        [&](std::uint64_t completed, std::uint64_t total, const std::string &path,
            const std::string &phase) {
          patch_state_["status"] = phase;
          patch_state_["completed_bytes"] = completed;
          patch_state_["total_bytes"] = total;
          patch_state_["current_file"] = path;
          if (phase == "cache_warning") {
            patch_state_["warning"] = path;
            log("Patch staging cleanup did not finish: " + path);
          }
          const auto now = std::chrono::steady_clock::now();
          if (phase != last_phase || now - last_progress >= std::chrono::milliseconds(150)) {
            last_progress = now;
            last_phase = phase;
            emit_state();
          }
        },
        patch_token,
        [&] {
          if (!find_client_processes(settings_.game_directory).empty()) {
            throw std::runtime_error("The game started during an update; installation stopped");
          }
        });
    patch_state_["status"] = "ready";
    patch_state_["error"] = "";
    patch_active_ = false;
    log("Game files verified for signed release " + patch_release_->version);
    emit_state();
  } catch (const std::exception &error) {
    patch_active_ = false;
    patch_state_["status"] = patch_token.stop_requested() ? "cancelled" : "error";
    patch_state_["error"] = error.what();
    emit_state();
    throw;
  }
}

void LauncherController::launch_game_client(const std::string &password) {
  refresh_client_status();
  if (client_) {
    throw std::runtime_error(
        "The game is already running. Close it before starting another session.");
  }

  if (!settings_.patch_public_key.empty()) {
    update_game();
  }
  refresh_server_status();
  refresh_client_status();
  std::filesystem::path bootstrap_archive;
  if (settings_.launch_mode == "named_account") {
    if (!server_running_ || !external_server_) {
      throw std::runtime_error("Start the persistent server before a named-account launch");
    }
    if (!health_.value("native_require_launch_grant", false)) {
      throw std::runtime_error(
          "Restart the persistent server with -RequireNamedLogin before a named-account launch");
    }
    bootstrap_archive = prepare_named_bootstrap(password);
  } else {
    if (!server_running_) {
      start_server();
    }
    bootstrap_archive = prepare_local_bootstrap();
  }

  const bool launching_new_client = !client_;

  if (settings_.client_launch_target == "managed") {
    const auto executable_path =
        std::filesystem::relative(expected_client_executable(settings_.game_directory),
                                  settings_.game_directory)
            .generic_string();
    if (!patch_release_ || std::none_of(patch_release_->files.begin(), patch_release_->files.end(),
                                        [&](const auto &file) {
                                          return _stricmp(file.path.c_str(),
                                                          executable_path.c_str()) == 0;
                                        })) {
      throw std::runtime_error("The signed release does not include the managed game executable");
    }
    require_managed_steam_compatibility(*patch_release_, settings_.game_directory);
    ensure_visual_cpp_runtime(launcher_directory_, settings_store_.directory() / "prerequisites");
    if (patch_release_->version == "0.2.0" && patch_release_->sequence == 2) {
      const auto backup = quarantine_managed_game_files(
          settings_.game_directory, legacy_game_runtime_files(launcher_directory_));
      if (!backup.empty()) {
        log("Moved the legacy runtime packaging files to a backup outside the game folder.");
      }
    }
    const auto client_pid =
        launch_managed_client(settings_.game_directory, settings_.steam_app_id, bootstrap_archive);
    // Zone access is unlocked from inside the game by the signed Steam compatibility library.
    log("Started the managed client with PID " + std::to_string(client_pid));
  } else if (settings_.client_launch_target == "private_copy") {
    launch_private_client(project_root_, settings_.game_directory, settings_.steam_app_id,
                          bootstrap_archive);
  } else {
    launch_through_steam(settings_.steam_app_id, bootstrap_archive);
  }
  bootstrap_launch_requested_ = launching_new_client;
  log("Launched " + settings_.client_launch_target + " client for " + settings_.launch_mode +
      " with a generated account bootstrap archive.");
}

std::filesystem::path LauncherController::prepare_local_bootstrap() {
  if (settings_.launch_mode != "disposable") {
    throw std::runtime_error("Select disposable mode to prepare a bootstrap without account login");
  }

  const auto launcher_certificate = settings_store_.directory() / "localhost.pem";
  const auto persistent_certificate = project_root_ / "out" / "native-server" / "localhost.pem";
  const auto certificate = std::filesystem::is_regular_file(launcher_certificate)
                               ? launcher_certificate
                               : persistent_certificate;
  return prepare_bootstrap_archive(project_root_, settings_store_.directory(), certificate,
                                   std::string(xorstr_("http://127.0.0.1:")) +
                                       std::to_string(settings_.http_port) + "/");
}

std::filesystem::path LauncherController::prepare_named_bootstrap(const std::string &password) {
  const auto base_url = service_url();
  if (!password.empty()) {
    account_session_.sign_in(base_url, settings_.account, password);
  }
  if (!account_session_.matches(base_url, settings_.account)) {
    throw std::runtime_error("Sign in to this account before launching the game.");
  }
  auto bootstrap_url = account_session_.launch_url();
  SecretClear clear_bootstrap_url{bootstrap_url};

  if (!settings_.patch_public_key.empty() ||
      !distribution::parse_service_origin(base_url).loopback) {
    if (!patch_release_ || !patch_release_->bootstrap) {
      throw std::runtime_error("The signed release does not contain a client bootstrap template");
    }
    return prepare_signed_bootstrap(*patch_release_, settings_store_.directory(), bootstrap_url,
                                    stop_source_.get_token());
  }

  const auto certificate = project_root_ / "out" / "native-server" / "localhost.pem";
  return prepare_bootstrap_archive(project_root_, settings_store_.directory(), certificate,
                                   bootstrap_url, true);
}

void LauncherController::apply_redirect() {
  apply_redirect_automatically();
  emit_state();
}

void LauncherController::apply_redirect_automatically() {
  if (settings_.launch_mode == "named_account") {
    throw std::runtime_error("Named accounts require a fresh launch through the launcher");
  }

  refresh_client_status();
  if (!client_) {
    throw std::runtime_error("The game client is not running");
  }

  client_memory_ = open_client_memory(client_->pid);
  AuthRedirector redirector(*client_memory_, std::string(xorstr_("http://127.0.0.1:")) +
                                                 std::to_string(settings_.http_port));

  auto manager = session_ ? std::optional<std::uint64_t>(session_->manager_address) : std::nullopt;
  if (manager && !redirector.is_manager_valid(*manager)) {
    manager.reset();
    reset_redirect_session();
    std::error_code error;
    std::filesystem::remove(session_cache_path(), error);
  }

  if (!manager) {
    // Full-process scans are expensive. After a failed discovery, back off instead of scanning
    // again on every status tick while the client is still booting.
    if (std::chrono::steady_clock::now() < next_discovery_attempt_) {
      redirect_status_ = "waiting";
      return;
    }

    redirect_status_ = "scanning";
    emit_state();
    manager = redirector.discover_manager(stop_source_.get_token(),
                                          [&](DiscoveryPhase, const std::string &detail) {
                                            redirect_detail_ = detail;
                                            log(detail);
                                            emit_state();
                                          });
    if (!manager) {
      next_discovery_attempt_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      redirect_detail_ = "The client has not exposed its region list yet; retrying shortly";
      redirect_status_ = "waiting";
      return;
    }
  }

  if (!session_) {
    session_ = RedirectSession{};
    session_->pid = client_->pid;
    session_->create_time = client_->create_time;
  }
  session_->manager_address = *manager;
  save_cached_session();

  const auto result = redirector.apply(*manager);
  for (const auto &target : result.patched) {
    session_->targets[target.header_address] = target;
    log("Redirected " + target.role + " to " + redirector.replacement_address());
  }
  for (const auto &failure : result.failures) {
    log("Redirect warning: " + failure);
  }

  // Reading the full inspection on every tick would add bridge traffic for no benefit; the patch
  // above already read and verified every endpoint it changed.
  last_inspection_ = redirector.inspect(*manager);
  redirect_status_ = session_->targets.empty() ? "idle" : "redirected";
  redirect_detail_ = session_->targets.empty() ? std::string{}
                                               : std::to_string(session_->targets.size()) +
                                                     " endpoint strings redirected in this session";
  if (!result.patched.empty()) {
    save_backup();
  }
  next_discovery_attempt_ = {};
}

void LauncherController::restore_redirect() {
  const int agent_engine_restored = restore_agent_engine_redirect();
  if ((!session_ || session_->targets.empty()) && agent_engine_restored > 0) {
    emit_state();
    return;
  }
  if (!session_ || session_->targets.empty()) {
    throw std::runtime_error("There is no launcher redirect to restore in this session");
  }
  if (!client_memory_) {
    if (!client_) {
      throw std::runtime_error("The game client is not running");
    }
    client_memory_ = open_client_memory(client_->pid);
  }

  AuthRedirector redirector(*client_memory_, std::string(xorstr_("http://127.0.0.1:")) +
                                                 std::to_string(settings_.http_port));
  std::vector<RedirectTarget> targets;
  for (const auto &[header, target] : session_->targets) {
    targets.push_back(target);
  }

  const auto result = redirector.restore(targets);
  log("Restored " + std::to_string(result.restored) + " endpoint strings");
  for (const auto &skipped : result.skipped) {
    log("Restore skipped " + skipped);
  }

  if (result.skipped.empty()) {
    reset_redirect_session();
    std::error_code error;
    std::filesystem::remove(session_cache_path(), error);
    if (client_) {
      std::filesystem::remove(backup_path_for_pid(client_->pid), error);
    }
  } else {
    // Keep the backup so Restore can be retried after investigating the skipped strings.
    redirect_status_ = "redirected";
    redirect_detail_ = std::to_string(result.skipped.size()) + " strings were left unchanged";
  }
  last_inspection_.reset();
  emit_state();
}

void LauncherController::restore_from_latest_backup() {
  const auto path = latest_backup_path();
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Could not read " + path.string());
  }

  const auto backup = json::parse(input);
  const auto pid = backup.at("pid").get<std::uint32_t>();
  const auto create_time = backup.at("create_time").get<std::int64_t>();
  if (query_process_create_time(pid) != create_time) {
    throw std::runtime_error("The client in that backup is no longer running");
  }

  auto memory = open_client_memory(pid);
  AuthRedirector redirector(*memory, backup.at("replacement").get<std::string>());
  std::vector<RedirectTarget> targets;
  for (const auto &entry : backup.at("targets")) {
    targets.push_back(target_from_json(entry));
  }

  const auto result = redirector.restore(targets);
  log("Backup restore: " + std::to_string(result.restored) + " restored, " +
      std::to_string(result.skipped.size()) + " skipped");
  for (const auto &skipped : result.skipped) {
    log("Restore skipped " + skipped);
  }
}

void LauncherController::save_settings(const json &changes) {
  auto updated = settings_;
  if (changes.contains("server_id")) {
    updated.server_id = changes.at("server_id").get<std::string>();
  }
  if (changes.contains("game_directory")) {
    updated.game_directory =
        std::filesystem::path(widen(changes.at("game_directory").get<std::string>()));
    updated.game_directory_explicit = !updated.game_directory.empty();
  }
  if (changes.contains("bridge_executable")) {
    updated.bridge_executable =
        std::filesystem::path(widen(changes.at("bridge_executable").get<std::string>()));
  }
  if (changes.contains("openssl_executable")) {
    updated.openssl_executable =
        std::filesystem::path(widen(changes.at("openssl_executable").get<std::string>()));
  }
  if (changes.contains("steam_app_id")) {
    updated.steam_app_id = changes.at("steam_app_id").get<std::string>();
  }
  if (changes.contains("backend_mode")) {
    updated.backend_mode = changes.at("backend_mode").get<std::string>();
    if (updated.backend_mode != "playsharp" && updated.backend_mode != "usermode") {
      throw std::invalid_argument("Backend must be playsharp or usermode");
    }
  }
  if (changes.contains("region")) {
    updated.region = changes.at("region").get<std::string>();
    if (updated.region.empty() || updated.region.size() > 32) {
      throw std::invalid_argument("Region must be 1-32 characters");
    }
  }
  if (changes.contains("account")) {
    updated.account = changes.at("account").get<std::string>();
    if (updated.account.size() < 3 || updated.account.size() > 32) {
      throw std::invalid_argument("Account must be 3-32 characters");
    }
  }
  if (changes.contains("launch_mode")) {
    updated.launch_mode = changes.at("launch_mode").get<std::string>();
    if (updated.launch_mode != "named_account" && updated.launch_mode != "disposable") {
      throw std::invalid_argument("Account mode must be named_account or disposable");
    }
  }
  if (changes.contains("client_launch_target")) {
    updated.client_launch_target = changes.at("client_launch_target").get<std::string>();
    if (updated.client_launch_target != "steam" && updated.client_launch_target != "private_copy" &&
        updated.client_launch_target != "managed") {
      throw std::invalid_argument("Client launch target must be steam, private_copy or managed");
    }
  }
  if (changes.contains("server_url")) {
    updated.server_url = changes.at("server_url").get<std::string>();
    if (!updated.server_url.empty()) {
      updated.server_url = distribution::parse_service_origin(updated.server_url).url;
    }
  }
  if (changes.contains("patch_public_key")) {
    updated.patch_public_key = changes.at("patch_public_key").get<std::string>();
  }
  if (changes.contains("patch_channel")) {
    updated.patch_channel = changes.at("patch_channel").get<std::string>();
  }
  if (changes.contains("http_port")) {
    updated.http_port = changes.at("http_port").get<int>();
  }
  if (changes.contains("world_port")) {
    updated.world_port = changes.at("world_port").get<int>();
  }
  if (changes.contains("world_enabled")) {
    updated.world_enabled = changes.at("world_enabled").get<bool>();
  }
  if (changes.contains("auto_start_server")) {
    updated.auto_start_server = changes.at("auto_start_server").get<bool>();
  }
  if (changes.contains("restore_on_exit")) {
    updated.restore_on_exit = changes.at("restore_on_exit").get<bool>();
  }
  if (changes.contains("stop_server_on_exit")) {
    updated.stop_server_on_exit = changes.at("stop_server_on_exit").get<bool>();
  }
  if (changes.contains("auto_redirect")) {
    updated.auto_redirect = changes.at("auto_redirect").get<bool>();
  }
  if (changes.contains("auto_world_entry")) {
    updated.auto_world_entry = changes.at("auto_world_entry").get<bool>();
  }
  if (changes.contains("agent_engine_port")) {
    updated.agent_engine_port = changes.at("agent_engine_port").get<int>();
  }
  if (changes.contains("agent_engine_redirect")) {
    updated.agent_engine_redirect = changes.at("agent_engine_redirect").get<bool>();
  }

  const bool restart_required =
      updated.http_port != settings_.http_port || updated.world_port != settings_.world_port ||
      updated.agent_engine_port != settings_.agent_engine_port ||
      updated.world_enabled != settings_.world_enabled || updated.region != settings_.region ||
      updated.account != settings_.account || updated.backend_mode != settings_.backend_mode ||
      updated.bridge_executable != settings_.bridge_executable ||
      updated.launch_mode != settings_.launch_mode ||
      updated.client_launch_target != settings_.client_launch_target ||
      updated.game_directory != settings_.game_directory;

  apply_distribution_profile(updated, launcher_directory_);
  if (live_servers_only_ && distribution::parse_service_origin(updated.server_url).loopback) {
    throw std::invalid_argument("The player launcher connects to live Project Chronicles servers only.");
  }
  validate_distribution_settings(updated);
  if (updated.game_directory != settings_.game_directory ||
      updated.server_url != settings_.server_url ||
      updated.patch_public_key != settings_.patch_public_key ||
      updated.patch_channel != settings_.patch_channel) {
    patch_check_requested_ = !updated.patch_public_key.empty();
    patch_release_.reset();
    patch_state_["status"] = updated.patch_public_key.empty() ? "disabled" : "unchecked";
  }
  settings_store_.save(updated);
  const bool realm_changed = updated.server_url != settings_.server_url ||
                             updated.http_port != settings_.http_port;
  if (realm_changed) {
    // Forget only the old realm's in-memory metadata; its Windows credential remains saved.
    account_session_.clear_realm();
  } else if (updated.account != settings_.account ||
             updated.launch_mode != settings_.launch_mode) {
    account_session_.sign_out();
  }
  if (updated.server_url != settings_.server_url || updated.http_port != settings_.http_port) {
    // Do not display another realm's population or content after a selection change.
    health_ = json::object();
    server_running_ = false;
    external_server_ = false;
    server_pid_ = 0;
    connection_attempts_ = 0;
    connection_error_.clear();
    news_feed_ = NewsFeed{};
    portal_feed_ = PortalFeed{};
  }
  settings_ = updated;
  if (realm_changed && settings_.launch_mode == "named_account") {
    account_session_.restore_saved_login(service_url(), {}, false);
  }
  configuration_failed_ = false;
  last_error_.clear();
  next_discovery_attempt_ = {};
  log("Settings saved" +
      (restart_required ? std::string{". Restart the server (and client redirect) to apply port, "
                                      "region or backend changes"}
                        : std::string{}));
  emit_state();
}

void LauncherController::select_server(const std::string &id) {
  refresh_client_status();
  if (client_ || patch_active_) {
    throw std::runtime_error("Close the game and finish its update before changing servers.");
  }
  const auto servers = release_servers(launcher_directory_);
  const auto selected = std::ranges::find(servers, id, &ReleaseServer::id);
  if (selected == servers.end() || (live_servers_only_ && selected->local)) {
    throw std::invalid_argument("Choose one of the available Project Chronicles servers.");
  }
  save_settings({{"server_id", id}});
  if (!initialized_) {
    initialize();
  }
}

void LauncherController::browse_game_directory() {
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(apartment)) {
    throw std::runtime_error("Could not open the game folder chooser.");
  }
  struct ApartmentGuard {
    ~ApartmentGuard() {
      CoUninitialize();
    }
  } guard;

  Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog)))) {
    throw std::runtime_error("Could not create the game folder chooser.");
  }
  dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  dialog->SetTitle(L"Choose the Project Chronicles game installation folder");
  const HRESULT shown = dialog->Show(nullptr);
  if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    return;
  }
  if (FAILED(shown)) {
    throw std::runtime_error("The game folder chooser failed.");
  }
  Microsoft::WRL::ComPtr<IShellItem> folder;
  PWSTR raw_path = nullptr;
  if (FAILED(dialog->GetResult(&folder)) ||
      FAILED(folder->GetDisplayName(SIGDN_FILESYSPATH, &raw_path))) {
    throw std::runtime_error("The chosen folder is not a local filesystem folder.");
  }
  const auto path = narrow(raw_path);
  CoTaskMemFree(raw_path);
  save_player_settings({{"game_directory", path}});
}

void LauncherController::save_player_settings(const json &changes) {
  if ((distribution_profile_ || player_interface_) &&
      (!changes.is_object() || changes.size() != 1 || !changes.contains("game_directory"))) {
    throw std::invalid_argument("Only the game folder can be changed in the player interface");
  }

  save_settings(changes);
}

void LauncherController::set_auto_redirect(bool enabled) {
  settings_.auto_redirect = enabled;
  settings_store_.save(settings_);
  if (!enabled) {
    redirect_status_ = session_ && !session_->targets.empty() ? "redirected" : "disabled";
    redirect_detail_ = "Automatic redirect is off";
  }
  log(enabled ? "Automatic redirect enabled" : "Automatic redirect disabled");
  emit_state();
}

void LauncherController::shutdown() {
  if (shutting_down_) {
    return;
  }
  account_session_.clear();
  shutting_down_ = true;

  if (settings_.restore_on_exit && session_ && !session_->targets.empty() && client_memory_) {
    try {
      AuthRedirector redirector(*client_memory_, std::string(xorstr_("http://127.0.0.1:")) +
                                                     std::to_string(settings_.http_port));
      std::vector<RedirectTarget> targets;
      for (const auto &[header, target] : session_->targets) {
        targets.push_back(target);
      }
      const auto result = redirector.restore(targets);
      log("Exit restore: " + std::to_string(result.restored) + " endpoint strings restored");
    } catch (const std::exception &error) {
      log(std::string("Exit restore failed: ") + error.what());
    }
  }
  if (settings_.restore_on_exit) {
    try {
      restore_agent_engine_redirect();
    } catch (const std::exception &error) {
      log(std::string("Agent engine exit restore failed: ") + error.what());
    }
  }

  if (settings_.stop_server_on_exit) {
    try {
      if (server_.is_running()) {
        server_.stop();
      }
    } catch (const std::exception &) {
      // The child handle is closed by ServerProcess either way.
    }
  }
  emit_state();
}

json LauncherController::state() const {
  auto account_state = account_session_.state();
  if (account_busy_) {
    account_state["status"] = "signing_in";
  }
  json regions = json::array();
  if (last_inspection_) {
    for (const auto &region : last_inspection_->regions) {
      regions.push_back(
          {{"name", region.name}, {"address", region.address}, {"redirected", region.redirected}});
    }
  }

  json assisted_clients = json::array();
  bool world_assist_running = false;
  std::uint32_t first_world_assist_pid = 0;
  for (const auto &[client_pid, session] : world_assists_) {
    assisted_clients.push_back({{"client_pid", client_pid},
                                {"running", session.running},
                                {"completed", session.connection_completed}});
    if (session.running) {
      world_assist_running = true;
      if (first_world_assist_pid == 0) {
        first_world_assist_pid = session.process->pid();
      }
    }
  }

  std::error_code client_file_error;
  const bool client_installed =
      !settings_.game_directory.empty() &&
      std::filesystem::is_regular_file(expected_client_executable(settings_.game_directory),
                                       client_file_error);

  return {
      {"settings", settings_to_json(settings_)},
      {"release_servers",
       [&] {
         json entries = json::array();
         for (const auto &server : release_servers(launcher_directory_)) {
           if (live_servers_only_ && server.local) {
             continue;
           }
           entries.push_back({{"id", server.id}, {"name", server.name}});
         }
         return entries;
       }()},
      {"distribution_profile", distribution_profile_},
      {"player_interface", distribution_profile_ || player_interface_},
      {"patch", patch_state_},
      {"patch_active", patch_active_},
      {"auth", account_state},
      {"news", news_feed_.state()},
      {"portal", portal_feed_.state()},
      {"launch",
       {{"status", launch_status_}, {"attempt", launch_attempt_}, {"error", launch_error_}}},
      {"readiness", {{"client_installed", client_installed}}},
      {"server",
       {{"running", server_running_},
        {"connection_attempts", connection_attempts_},
        {"connection_error", connection_error_},
        {"managed", server_running_ && !external_server_},
        {"pid", server_pid_},
        {"health", health_},
        {"http_port", settings_.http_port},
        {"url", service_url()},
        {"world_port", settings_.world_enabled ? settings_.world_port : 0},
        {"account", settings_.account},
        {"region", settings_.region},
        {"log", server_log_tail_}}},
      {"client",
       {{"running", client_.has_value()},
        {"pid", client_ ? client_->pid : 0},
        {"pids", client_pids_},
        {"path", client_ ? narrow(client_->executable.wstring()) : std::string{}},
        {"expected_executable",
         narrow(expected_client_executable(settings_.game_directory).wstring())}}},
      {"bridge",
       {{"running", bridge_.is_running()},
        {"backend", settings_.backend_mode},
        {"configured_path", narrow(settings_.bridge_executable.wstring())}}},
      {"redirect",
       {{"status", redirect_status_},
        {"detail", redirect_detail_},
        {"auto", settings_.auto_redirect},
        {"manager", session_ ? format_address(session_->manager_address) : std::string{}},
        {"patched", session_ ? session_->targets.size() : 0},
        {"selected_region", last_inspection_ ? last_inspection_->selected_region : std::string{}},
        {"current_address", last_inspection_ ? last_inspection_->current_address : std::string{}},
        {"regions", regions}}},
      {"agent_engine",
       {{"status", agent_engine_status_},
        {"enabled", settings_.agent_engine_redirect && settings_.agent_engine_port != 0},
        {"port", settings_.agent_engine_port},
        {"address",
         agent_engine_session_ ? agent_engine_session_->replacement_address : std::string{}},
        {"patched", agent_engine_session_ ? agent_engine_session_->targets.size() : 0}}},
      {"world_entry",
       {{"enabled", settings_.auto_world_entry},
        {"running", world_assist_running},
        {"pid", first_world_assist_pid},
        {"clients", assisted_clients},
        {"log", world_assist_log_tail_}}},
      {"activity", activity_},
      {"error", last_error_},
  };
}

void LauncherController::log(std::string message) {
  const auto line = "[" + timestamp() + "] " + message;
  activity_.push_back(line);
  if (activity_.size() > 250) {
    activity_.erase(activity_.begin(), activity_.begin() + 50);
  }

  try {
    std::filesystem::create_directories(settings_store_.directory());
    std::ofstream file(settings_store_.directory() / "launcher.log", std::ios::app);
    file << line << "\n";
  } catch (const std::exception &) {
    // Logging must never break a launcher action.
  }
}

void LauncherController::note_error(const std::string &message) {
  last_error_ = message;
  log("Error: " + message);
  emit_state();
}

void LauncherController::request_stop() {
  stop_source_.request_stop();
  cancel_update();
}

bool LauncherController::stop_requested() const {
  return stop_source_.stop_requested();
}

void LauncherController::emit_state() {
  if (state_sink_) {
    state_sink_(state().dump());
  }
}

void LauncherController::refresh_server_status() {
  const bool managed_running = server_.is_running();
  try {
    health_ = health_request_(service_url() + "/health");
    server_running_ = health_.value("native_login_adapter", false);
    if (!server_running_) {
      throw std::runtime_error("The selected server is not ready");
    }
    connection_attempts_ = 0;
    connection_error_.clear();
    external_server_ = server_running_ && !managed_running;
    server_pid_ = managed_running ? server_.pid() : 0;
  } catch (const std::exception &) {
    constexpr unsigned kMaximumConnectionAttempts = 3;
    connection_attempts_ = std::min(connection_attempts_ + 1, kMaximumConnectionAttempts);
    if (connection_attempts_ >= kMaximumConnectionAttempts) {
      connection_error_ = "Failed to connect to the server. Choose another server or try again.";
    }
    health_ = json::object();
    server_running_ = false;
    external_server_ = false;
    server_pid_ = managed_running ? server_.pid() : 0;
  }
}

void LauncherController::refresh_client_status() {
  const auto previous_pid = client_ ? client_->pid : 0;
  const auto clients = find_client_processes(settings_.game_directory);
  client_pids_.clear();
  for (const auto &client : clients) {
    client_pids_.push_back(client.pid);
  }
  client_ = clients.empty() ? std::nullopt : std::optional<ClientProcess>(clients.front());
  if (!client_) {
    if (launch_status_ == "started" && std::chrono::steady_clock::now() >= launch_deadline_) {
      launch_status_ = "failed";
      launch_error_ =
          "The game did not appear within 90 seconds. Check launcher activity and retry.";
    }
    if (previous_pid != 0) {
      log("Client exited; its runtime redirect ended with the process");
      bootstrap_launch_requested_ = false;
      launch_status_ = "idle";
    }
    reset_redirect_session();
    client_memory_.reset();
    last_inspection_.reset();
    std::error_code error;
    std::filesystem::remove(session_cache_path(), error);
    session_cache_attempted_ = false;
    if (redirect_status_ != "disabled") {
      redirect_status_ = settings_.auto_redirect ? "waiting" : "idle";
      redirect_detail_ = "Waiting for the game client";
    }
    return;
  }

  if (launch_status_ == "started") {
    launch_status_ = "running";
  }
  if (session_ &&
      (session_->pid != client_->pid || session_->create_time != client_->create_time)) {
    log("Client restarted; rediscovering its region list");
    reset_redirect_session();
    session_cache_attempted_ = false;
  }
  if (!session_ && !session_cache_attempted_) {
    session_cache_attempted_ = true;
    load_cached_session();
  }
  client_memory_.reset();
}

void LauncherController::refresh_redirect() {
  if (!client_ || !settings_.auto_redirect || shutting_down_) {
    return;
  }

  if (bootstrap_launch_requested_) {
    redirect_status_ = "bootstrap";
    redirect_detail_ = "The generated bootstrap points this client at the local server";
    return;
  }

  if (settings_.launch_mode == "named_account") {
    redirect_status_ = "waiting";
    redirect_detail_ = "Launch through the named-account sign-in to select your account";
    return;
  }

  try {
    apply_redirect();
    last_error_.clear();
  } catch (const std::exception &error) {
    redirect_status_ = "waiting";
    redirect_detail_ = error.what();
  }
}

void LauncherController::reset_redirect_session() {
  session_.reset();
  last_inspection_.reset();
  if (redirect_status_ != "disabled") {
    redirect_status_ = settings_.auto_redirect ? "waiting" : "idle";
  }
  redirect_detail_.clear();
}

std::string LauncherController::agent_engine_url() const {
  return agent_engine_address(service_url(), settings_.agent_engine_port);
}

void LauncherController::refresh_agent_engine_redirect() {
  if (!client_ || shutting_down_ || !settings_.agent_engine_redirect ||
      settings_.agent_engine_port == 0) {
    return;
  }

  // A restarted client allocates fresh strings; its predecessor's targets no longer apply.
  if (agent_engine_session_ && (agent_engine_session_->pid != client_->pid ||
                                agent_engine_session_->create_time != client_->create_time)) {
    agent_engine_session_.reset();
  }
  if (!agent_engine_session_) {
    agent_engine_session_ = AgentEngineRedirectSession{};
    agent_engine_session_->pid = client_->pid;
    agent_engine_session_->create_time = client_->create_time;
    agent_engine_session_->replacement_address = agent_engine_url();
  }

  auto &session = *agent_engine_session_;
  const auto now = std::chrono::steady_clock::now();
  if (session.redirected || now < session.next_attempt) {
    return;
  }

  // One full-memory scan per attempt; retry slowly while the client is still starting.
  session.next_attempt = now + std::chrono::seconds(30);
  try {
    auto memory = open_client_memory(client_->pid);
    AgentEngineRedirector redirector(*memory, session.replacement_address);
    auto result = redirector.apply(stop_source_.get_token());
    for (auto &target : result.patched) {
      log("Redirected the agent engine address to " + session.replacement_address);
      session.targets.push_back(std::move(target));
    }
    for (const auto &failure : result.failures) {
      log("Agent engine redirect warning: " + failure);
    }

    session.redirected = !session.targets.empty() || result.already_redirected > 0;
    agent_engine_status_ = session.redirected ? "redirected" : "waiting";
    if (!result.patched.empty()) {
      save_agent_engine_backup();
    }
  } catch (const OperationCancelled &) {
    agent_engine_status_ = "waiting";
  } catch (const std::exception &error) {
    if (agent_engine_status_ != "error") {
      log(std::string("Agent engine redirect unavailable: ") + error.what());
    }
    agent_engine_status_ = "error";
  }
}

int LauncherController::restore_agent_engine_redirect() {
  if (!agent_engine_session_ || agent_engine_session_->targets.empty() || !client_) {
    return 0;
  }

  auto memory = open_client_memory(client_->pid);
  AgentEngineRedirector redirector(*memory, agent_engine_session_->replacement_address);
  const auto result = redirector.restore(agent_engine_session_->targets);
  log("Restored " + std::to_string(result.restored) + " agent engine address strings");
  for (const auto &skipped : result.skipped) {
    log("Restore skipped " + skipped);
  }

  if (result.skipped.empty()) {
    // Stay "redirected" for this client so the next refresh does not patch it again.
    agent_engine_session_->targets.clear();
    agent_engine_status_ = "restored";
    std::error_code error;
    std::filesystem::remove(agent_engine_backup_path(agent_engine_session_->pid), error);
  }
  return result.restored;
}

std::filesystem::path LauncherController::agent_engine_backup_path(std::uint32_t pid) const {
  return settings_store_.directory() / ("agent-engine-backup-" + std::to_string(pid) + ".json");
}

void LauncherController::save_agent_engine_backup() {
  if (!agent_engine_session_) {
    return;
  }

  json targets = json::array();
  for (const auto &target : agent_engine_session_->targets) {
    targets.push_back(target_to_json(target));
  }
  const json backup{{"pid", agent_engine_session_->pid},
                    {"create_time", agent_engine_session_->create_time},
                    {"replacement", agent_engine_session_->replacement_address},
                    {"targets", targets}};

  std::filesystem::create_directories(settings_store_.directory());
  std::ofstream output(agent_engine_backup_path(agent_engine_session_->pid), std::ios::trunc);
  output << backup.dump(2) << "\n";
}

void LauncherController::ensure_bridge() {
  if (bridge_.is_running()) {
    return;
  }

  log("Starting the PlaySharp bridge (" + settings_.backend_mode + " backend)");
  bridge_.start(settings_.bridge_executable);
}

std::unique_ptr<ProcessMemory> LauncherController::open_client_memory(std::uint32_t pid) {
  if (settings_.backend_mode == "usermode") {
    return std::make_unique<UsermodeProcessMemory>(
        pid, expected_client_executable(settings_.game_directory));
  }

  ensure_bridge();
  return std::make_unique<BridgeProcessMemory>(bridge_, pid, settings_.backend_mode);
}

ServerLaunchOptions LauncherController::server_options() const {
  ServerLaunchOptions options;
  options.server_executable =
      development_release_directory(project_root_) / "chronicles-server.exe";
  options.project_root = project_root_;
  options.run_directory = project_root_ / "out" / "launcher";
  options.protocol_directory = project_root_ / "protocol";
  const auto client_data_directory = project_root_ / "data" / "generated" / "catalog";
  if (std::filesystem::is_regular_file(client_data_directory / "manifest.json")) {
    options.client_data_directory = client_data_directory;
  }
  options.openssl_executable = settings_.openssl_executable;
  options.region = settings_.region;
  options.account = settings_.account;
  options.http_port = settings_.http_port;
  options.world_port = settings_.world_port;
  options.agent_engine_port = settings_.agent_engine_port;
  options.world_enabled = settings_.world_enabled;
  options.world_entry_diagnostic = settings_.auto_world_entry;
  return options;
}

void LauncherController::refresh_world_assist() {
  const bool hosted_world = !settings_.server_url.empty() &&
                            !distribution::parse_service_origin(settings_.server_url).loopback;
  const auto now = std::chrono::steady_clock::now();
  for (auto session_it = world_assists_.begin(); session_it != world_assists_.end();) {
    const auto client_pid = session_it->first;
    auto &session = session_it->second;
    const bool running = session.process->is_running();
    if (session.running && !running) {
      session.connection_completed = session.process->last_exit_code() == 0;
      log("World entry helper for client PID " + std::to_string(client_pid) +
          (session.connection_completed ? " completed its connection trial"
                                        : " ended; inspect its log"));
      if (hosted_world && !session.connection_completed) {
        // A rejected hosted certificate must not trigger repeated client-memory changes.
        session.next_attempt = std::chrono::steady_clock::time_point::max();
        log("Hosted world entry stopped for this client; restart the game after reviewing "
            "the helper log");
      } else {
        session.next_attempt = now + std::chrono::seconds(5);
      }
    }
    session.running = running;

    if (!running && query_process_create_time(client_pid) != session.client_create_time) {
      session_it = world_assists_.erase(session_it);
      continue;
    }
    ++session_it;
  }

  if (!settings_.auto_world_entry || !settings_.world_enabled || !server_running_ ||
      shutting_down_ || stop_requested()) {
    return;
  }

  auto clients = find_client_processes(settings_.game_directory);
  if (settings_.client_launch_target == "steam" && clients.size() > 1) {
    clients.resize(1);
  }

  const auto launcher_create_time = query_process_create_time(GetCurrentProcessId());
  auto executable = launcher_directory_ / "chronicles-world-tls-local.exe";
  if (!std::filesystem::is_regular_file(executable)) {
    // Developer checkouts keep the helper in the player launcher's Release output folder.
    executable = development_release_directory(project_root_) / "launcher" /
                 "chronicles-world-tls-local.exe";
  }
  for (const auto &client : clients) {
    auto [session_it, inserted] = world_assists_.try_emplace(client.pid);
    auto &session = session_it->second;
    if (inserted || session.client_create_time != client.create_time) {
      if (session.running) {
        continue;
      }
      session = WorldAssistSession{};
      session.client_create_time = client.create_time;
    }
    if (session.running || now < session.next_attempt) {
      continue;
    }
    const bool existing_client =
        inserted && launcher_create_time != 0 && client.create_time < launcher_create_time;
    if (session.connection_completed || existing_client) {
      try {
        const WorldConnectionTarget target(settings_.world_host, settings_.world_port);
        if (target.has_connection(client.pid)) {
          session.connection_completed = true;
          continue;
        }
      } catch (const std::exception &error) {
        log("Cannot check the selected world for client PID " + std::to_string(client.pid) + ": " +
            error.what());
        session.next_attempt = now + std::chrono::seconds(30);
        continue;
      }
    }

    session.connection_completed = false;
    const auto log_path =
        settings_store_.directory() / ("world-entry-" + std::to_string(client.pid) + ".log");
    try {
      session.process->start(executable, project_root_, log_path, client.pid,
                             settings_.game_directory, settings_.backend_mode,
                             hosted_world ? settings_.world_host : std::string{},
                             hosted_world ? settings_.world_port : 0);
      session.running = true;
      log("Armed guarded world entry for client PID " + std::to_string(client.pid));
    } catch (const std::exception &error) {
      log("World entry helper for client PID " + std::to_string(client.pid) +
          " could not start: " + error.what());
      session.next_attempt = now + std::chrono::seconds(30);
    }
  }
}

void LauncherController::save_backup() {
  if (!session_) {
    return;
  }

  json targets = json::array();
  for (const auto &[header, target] : session_->targets) {
    targets.push_back(target_to_json(target));
  }

  const json backup{{"pid", session_->pid},
                    {"create_time", session_->create_time},
                    {"replacement", std::string(xorstr_("http://127.0.0.1:")) +
                                        std::to_string(settings_.http_port)},
                    {"targets", targets}};

  std::filesystem::create_directories(settings_store_.directory());
  const auto path =
      settings_store_.directory() / ("runtime-backup-" + std::to_string(session_->pid) + ".json");
  std::ofstream output(path, std::ios::trunc);
  output << backup.dump(2) << "\n";
}

void LauncherController::load_cached_session() {
  if (!client_) {
    return;
  }

  try {
    std::ifstream input(session_cache_path());
    if (!input) {
      return;
    }

    const auto cached = json::parse(input);
    if (cached.at("pid").get<std::uint32_t>() != client_->pid ||
        cached.at("create_time").get<std::int64_t>() != client_->create_time) {
      return;
    }

    session_ = RedirectSession{};
    session_->pid = client_->pid;
    session_->create_time = client_->create_time;
    session_->manager_address = parse_address(cached.at("manager").get<std::string>());

    // Reattach the previous run's original-byte backups so Restore still works after the
    // launcher is restarted. The manager is revalidated before any write.
    std::ifstream backup_input(backup_path_for_pid(client_->pid));
    if (backup_input) {
      const auto backup = json::parse(backup_input);
      if (backup.at("create_time").get<std::int64_t>() == client_->create_time &&
          backup.at("replacement").get<std::string>() ==
              std::string(xorstr_("http://127.0.0.1:")) + std::to_string(settings_.http_port)) {
        for (const auto &entry : backup.at("targets")) {
          auto target = target_from_json(entry);
          session_->targets[target.header_address] = std::move(target);
        }
      }
    }
    log("Reusing the validated client session from the previous launcher run");
  } catch (const std::exception &) {
    // A stale or corrupt cache only costs one fresh discovery.
    session_.reset();
  }
}

void LauncherController::save_cached_session() {
  if (!session_) {
    return;
  }

  const json cached{{"pid", session_->pid},
                    {"create_time", session_->create_time},
                    {"manager", format_address(session_->manager_address)}};
  std::filesystem::create_directories(settings_store_.directory());
  std::ofstream output(session_cache_path(), std::ios::trunc);
  output << cached.dump(2) << "\n";
}

std::filesystem::path LauncherController::session_cache_path() const {
  return settings_store_.directory() / "client-session.json";
}

std::filesystem::path LauncherController::backup_path_for_pid(std::uint32_t pid) const {
  return settings_store_.directory() / ("runtime-backup-" + std::to_string(pid) + ".json");
}

std::filesystem::path LauncherController::latest_backup_path() const {
  std::optional<std::filesystem::path> latest;
  std::filesystem::file_time_type latest_time{};
  for (const auto &entry : std::filesystem::directory_iterator(settings_store_.directory())) {
    if (!entry.is_regular_file() || entry.path().filename().string().find("runtime-backup-") != 0) {
      continue;
    }

    const auto modified = entry.last_write_time();
    if (!latest || modified > latest_time) {
      latest = entry.path();
      latest_time = modified;
    }
  }

  if (!latest) {
    throw std::runtime_error("No runtime redirect backup exists");
  }
  return *latest;
}

} // namespace chronicles::launcher
