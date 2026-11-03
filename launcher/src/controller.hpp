#pragma once

#include "account_session.hpp"
#include "agent_engine_redirect.hpp"
#include "auth_redirect.hpp"
#include "game_process.hpp"
#include "news_feed.hpp"
#include "patch_installer.hpp"
#include "playsharp_bridge.hpp"
#include "portal_feed.hpp"
#include "server_process.hpp"
#include "settings.hpp"
#include "world_assist_process.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace chronicles::launcher {

// Owns the launcher's runtime state. All methods run on the controller's worker thread (or on the
// main thread in CLI mode); the UI only sends actions and receives serialized state.
class LauncherController {
public:
  using HealthRequest = std::function<nlohmann::json(const std::string &)>;

  LauncherController(std::filesystem::path project_root, std::filesystem::path launcher_directory,
                     bool live_servers_only = false, HealthRequest health_request = {},
                     std::wstring credential_target_prefix = L"Project Chronicles Launcher/");

  void set_state_sink(std::function<void(const std::string &state_json)> sink);

  // A newly attached view must receive state even after initialization failed.
  void publish_state();

  void initialize();

  // Read-only status for the CLI: never starts a server or scans the client.
  void probe();

  void refresh();

  void refresh_news();

  void refresh_content();

  void refresh_characters();

  std::string portal_link(const std::string &kind, const std::string &id) const;

  void start_server();

  void stop_server();

  void launch_game(std::string password = {}, const std::string &account = {});

  void sign_in(std::string password, const std::string &account, bool keep_signed_in = false,
               bool remember_password = false, bool use_saved_password = false);

  void set_login_preferences(bool keep_signed_in, bool remember_password);

  void forget_saved_login();

  void sign_out();

  std::string begin_discord_login(bool keep_signed_in);
  void cancel_discord_login();

  std::string register_account(std::string password, const std::string &account);

  void check_updates(std::stop_token stop = {}, bool verify_files = false,
                     bool inspect_files = true);

  void update_game(bool verify_files = false);

  void cancel_update();

  std::filesystem::path prepare_local_bootstrap();

  std::filesystem::path prepare_named_bootstrap(const std::string &password);

  void apply_redirect();

  void restore_redirect();

  void restore_from_latest_backup();

  void save_settings(const nlohmann::json &changes);

  void select_server(const std::string &id);

  void browse_game_directory();

  void save_player_settings(const nlohmann::json &changes);

  void set_auto_redirect(bool enabled);

  void shutdown();

  nlohmann::json state() const;

  void log(std::string message);

  void note_error(const std::string &message);

  // Asks long discovery scans to stop so the window can close promptly.
  void request_stop();

  bool stop_requested() const;

private:
  void launch_game_client(const std::string &password);

  std::string service_url() const;

  struct RedirectSession {
    std::uint32_t pid{};
    std::int64_t create_time{};
    std::uint64_t manager_address{};
    std::map<std::uint64_t, RedirectTarget> targets;
  };

  struct WorldAssistSession {
    std::unique_ptr<WorldAssistProcess> process{std::make_unique<WorldAssistProcess>()};
    std::int64_t client_create_time{};
    bool running{};
    bool connection_completed{};
    std::chrono::steady_clock::time_point next_attempt{};
  };

  // The client's agent-engine (AI chat) base URL redirect for one client process.
  struct AgentEngineRedirectSession {
    std::uint32_t pid{};
    std::int64_t create_time{};
    std::string replacement_address;
    std::vector<RedirectTarget> targets;
    bool redirected{};
    std::chrono::steady_clock::time_point next_attempt{};
  };

  void emit_state();

  void refresh_server_status();

  void refresh_client_status();

  void refresh_redirect();

  void refresh_agent_engine_redirect();

  // Puts the client's original agent-engine address back; returns the strings restored.
  int restore_agent_engine_redirect();

  // Original-byte backup of the agent-engine redirect, beside the auth runtime backup.
  void save_agent_engine_backup();

  std::filesystem::path agent_engine_backup_path(std::uint32_t pid) const;

  // Address written into the client: this server's host and the agent-engine port.
  std::string agent_engine_url() const;

  void refresh_world_assist();

  void apply_redirect_automatically();

  void reset_redirect_session();

  void ensure_bridge();

  std::unique_ptr<ProcessMemory> open_client_memory(std::uint32_t pid);

  ServerLaunchOptions server_options() const;

  void save_backup();

  void load_cached_session();

  void save_cached_session();

  std::filesystem::path session_cache_path() const;

  std::filesystem::path backup_path_for_pid(std::uint32_t pid) const;

  std::filesystem::path latest_backup_path() const;

  std::filesystem::path project_root_;
  std::filesystem::path launcher_directory_;
  SettingsStore settings_store_;
  AccountSession account_session_;
  NewsFeed news_feed_;
  PortalFeed portal_feed_;
  LauncherSettings settings_;
  ServerProcess server_;
  std::map<std::uint32_t, WorldAssistSession> world_assists_;
  PlaySharpBridge bridge_;
  std::unique_ptr<ProcessMemory> client_memory_;
  std::function<void(const std::string &)> state_sink_;

  bool initialized_{};
  bool shutting_down_{};
  bool bootstrap_launch_requested_{};
  bool server_running_{};
  HealthRequest health_request_;
  unsigned connection_attempts_{};
  std::string connection_error_;
  bool external_server_{};
  std::uint32_t server_pid_{};
  nlohmann::json health_{nlohmann::json::object()};
  std::optional<ClientProcess> client_;
  std::vector<std::uint32_t> client_pids_;
  std::optional<RedirectSession> session_;
  std::optional<AgentEngineRedirectSession> agent_engine_session_;
  std::string agent_engine_status_{"idle"};
  std::string redirect_status_{"idle"};
  std::string redirect_detail_;
  std::string last_error_;
  std::string launch_status_{"idle"};
  std::string launch_error_;
  std::chrono::steady_clock::time_point launch_deadline_{};
  std::uint64_t launch_attempt_{};
  bool account_busy_{};
  bool distribution_profile_{};
  bool player_interface_{};
  bool live_servers_only_{};
  bool configuration_failed_{};
  bool patch_active_{};
  bool patch_check_requested_{};
  nlohmann::json patch_state_{{"status", "disabled"}, {"completed_bytes", 0}, {"total_bytes", 0}};
  std::optional<distribution::Release> patch_release_;
  std::stop_source patch_stop_;
  std::mutex patch_stop_mutex_;
  std::vector<std::string> activity_;
  std::optional<AuthManagerState> last_inspection_;
  std::string server_log_tail_;
  std::string world_assist_log_tail_;
  int ticks_since_log_read_{};
  std::chrono::steady_clock::time_point next_discovery_attempt_{};
  std::chrono::steady_clock::time_point next_inspection_{};
  std::stop_source stop_source_;
  bool session_cache_attempted_{};
};

} // namespace chronicles::launcher
