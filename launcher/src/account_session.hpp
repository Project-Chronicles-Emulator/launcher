#pragma once

#include "credential_store.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace chronicles::launcher {

class DiscordLoopback;

// A launcher login is distinct from the native game's one-use grant/session. Credentials stay
// in the native host; only public status is serialized to WebView2. This class is worker-owned.
class AccountSession {
public:
  using TimePoint = std::chrono::steady_clock::time_point;
  using Now = std::function<TimePoint()>;
  using Transport =
      std::function<nlohmann::json(const std::string &method, const std::string &url,
                                   const nlohmann::json &body, const std::string &token)>;

  explicit AccountSession(Transport transport = {}, Now now = {},
                          std::filesystem::path credential_path = {},
                          std::wstring credential_target_prefix = L"Project Chronicles Launcher/");
  ~AccountSession();

  AccountSession(const AccountSession &) = delete;
  AccountSession &operator=(const AccountSession &) = delete;

  void sign_in(const std::string &origin, std::string account, const std::string &password,
               bool keep_signed_in = false, bool remember_password = false,
               bool use_saved_password = false);
  std::string register_account(const std::string &origin, std::string account,
                               const std::string &password);
  void refresh_discord_options(const std::string &origin);
  std::string begin_discord_login(const std::string &origin, bool keep_signed_in);
  void tick_discord_login(bool server_online);
  void cancel_discord_login();
  void restore_saved_login(const std::string &origin, const std::string &account,
                           bool validate_immediately = true);
  void set_login_preferences(bool keep_signed_in, bool remember_password);
  void forget_saved_login();
  void sign_out();
  void clear();
  // Leave a realm without deleting its independently stored credentials.
  void clear_realm();
  void refresh(bool force = false);
  void refresh_characters(bool force = false);
  bool matches(const std::string &origin, const std::string &account) const;
  std::string launch_url();
  nlohmann::json state() const;

private:
  void expire();
  void resume_with_saved_password();
  void save_login(bool keep_signed_in, bool remember_password, const std::string &password = {});

  CredentialStore credential_store_;
  Transport transport_;
  Now now_;
  std::string origin_;
  std::string account_;
  std::string token_;
  std::string status_{"signed_out"};
  std::string error_;
  bool connected_{};
  std::uint64_t attempt_{};
  TimePoint next_validation_{};
  TimePoint next_roster_refresh_{};
  nlohmann::json characters_ = nlohmann::json::array();
  std::string character_status_{"signed_out"};
  std::string character_error_;
  std::string saved_origin_;
  std::string saved_account_;
  std::string persistence_error_;
  bool keep_signed_in_{};
  bool remember_password_{};
  bool auto_resume_pending_{};
  bool authentication_rejected_{};
  std::string discord_origin_;
  std::string discord_request_id_;
  std::string discord_poll_secret_;
  std::string discord_browser_proof_;
  std::unique_ptr<DiscordLoopback> discord_loopback_;
  std::string discord_error_;
  std::string discord_result_;
  TimePoint discord_deadline_{};
  TimePoint discord_next_poll_{};
  TimePoint discord_next_options_{};
  bool discord_enabled_{};
  bool discord_keep_signed_in_{};
  bool discord_linking_{};
};

} // namespace chronicles::launcher
