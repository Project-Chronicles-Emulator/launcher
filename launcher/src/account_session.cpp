#include "account_session.hpp"
#include "discord_loopback.hpp"
#include "xorstr.hpp"

#include "distribution/distribution.hpp"
#include "http_client.hpp"
#include "auth/password_policy.hpp"

#include <windows.h>

#include <algorithm>
#include <stdexcept>

namespace chronicles::launcher {
namespace {

using json = nlohmann::json;
constexpr auto kValidationInterval = std::chrono::seconds(30);

struct ClearPassword {
  json &credentials;

  ~ClearPassword() {
    auto &secret = credentials.at("password").get_ref<std::string &>();
    SecureZeroMemory(secret.data(), secret.size());
  }
};

std::string normalize_account(std::string account) {
  if (account.size() < 3 || account.size() > 32) {
    throw std::invalid_argument("Account names must contain 3–32 letters, digits or underscores.");
  }
  for (auto &character : account) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character + ('a' - 'A'));
    }
    if ((character < 'a' || character > 'z') && (character < '0' || character > '9') &&
        character != '_') {
      throw std::invalid_argument(
          "Account names must contain 3–32 letters, digits or underscores.");
    }
  }
  return account;
}

json send_request(const std::string &method, const std::string &url, const json &body,
                  const std::string &token) {
  if (method == "GET") {
    return http_get_json(url, 3000, token);
  }
  return http_post_json(url, body, token, 5000);
}

} // namespace

AccountSession::AccountSession(Transport transport, Now now, std::filesystem::path credential_path,
                               std::wstring credential_target_prefix)
    : credential_store_(std::move(credential_path), std::move(credential_target_prefix)),
      transport_(transport ? std::move(transport) : send_request),
      now_(now ? std::move(now) : [] { return std::chrono::steady_clock::now(); }) {}

AccountSession::~AccountSession() {
  clear();
}

void AccountSession::clear() {
  cancel_discord_login();
  discord_error_.clear();
  discord_result_.clear();
  if (!token_.empty()) {
    SecureZeroMemory(token_.data(), token_.size());
    token_.clear();
  }
  origin_.clear();
  account_.clear();
  status_ = "signed_out";
  error_.clear();
  connected_ = false;
  characters_ = json::array();
  character_status_ = "signed_out";
  character_error_.clear();
  next_roster_refresh_ = {};
  auto_resume_pending_ = false;
}

void AccountSession::clear_realm() {
  clear();
  keep_signed_in_ = false;
  remember_password_ = false;
  saved_origin_.clear();
  saved_account_.clear();
  persistence_error_.clear();
}

void AccountSession::sign_in(const std::string &origin, std::string account,
                             const std::string &password, bool keep_signed_in,
                             bool remember_password, bool use_saved_password) {
  ++attempt_;
  clear();
  status_ = "signing_in";
  authentication_rejected_ = false;
  try {
    account = normalize_account(std::move(account));
    const auto validated_origin = distribution::parse_service_origin(origin).url;
    std::unique_ptr<SavedLogin> saved;
    if (use_saved_password && password.empty()) {
      saved = credential_store_.load(validated_origin, account);
    }
    const auto &effective_password = saved ? saved->password : password;
    if (effective_password.empty() || effective_password.size() > 256) {
      throw std::invalid_argument("Enter your password (up to 256 bytes).");
    }
    json credentials{
        {"account", account}, {"password", effective_password}, {"client", "launcher"}};
    ClearPassword clear_password{credentials};
    auto reply =
        transport_("POST", validated_origin + xorstr_("/emulator/v1/login"), credentials, {});
    if (!reply.contains("access_token") || !reply.at("access_token").is_string()) {
      authentication_rejected_ = true;
      throw std::runtime_error("Account name or password is incorrect. Please try again.");
    }
    token_ = std::move(reply.at("access_token").get_ref<std::string &>());
    if (!distribution::valid_sha256(token_)) {
      throw std::runtime_error("The realm returned an invalid sign-in response.");
    }
    origin_ = validated_origin;
    account_ = std::move(account);
    status_ = "signed_in";
    connected_ = true;
    next_validation_ = now_() + kValidationInterval;
    character_status_ = "loading";
    try {
      save_login(keep_signed_in, remember_password, effective_password);
    } catch (const std::exception &error) {
      keep_signed_in_ = false;
      remember_password_ = false;
      saved_origin_.clear();
      saved_account_.clear();
      persistence_error_ = error.what();
    }
  } catch (const HttpError &error) {
    clear();
    status_ = "error";
    if (error.status() == 429) {
      error_ = "Too many sign-in attempts. Wait a moment and try again.";
    } else if (error.status() == 401 || error.status() == 403) {
      authentication_rejected_ = true;
      error_ = "Account name or password is incorrect. Please try again.";
    } else {
      error_ = "Cannot connect to the realm. Check your connection and try again.";
    }
    throw std::runtime_error(error_);
  } catch (const json::exception &) {
    clear();
    status_ = "error";
    error_ = "The realm returned an invalid sign-in response. Please try again.";
    throw std::runtime_error(error_);
  } catch (const std::exception &error) {
    clear();
    status_ = "error";
    error_ = error.what();
    throw;
  }
}

std::string AccountSession::register_account(const std::string &origin, std::string account,
                                             const std::string &password) {
  if (!token_.empty()) {
    throw std::runtime_error("Sign out before creating another account.");
  }
  account = normalize_account(std::move(account));
  const auto validated_origin = distribution::parse_service_origin(origin).url;
  if (!is_valid_new_password(password)) {
    throw std::invalid_argument("Choose a password containing 8–256 bytes.");
  }

  json credentials{{"account", account}, {"password", password}};
  // Creating an account never authenticates or saves its password.
  ClearPassword clear_password{credentials};

  try {
    const auto reply =
        transport_("POST", validated_origin + xorstr_("/emulator/v1/register"), credentials, {});
    if (!reply.is_object() || !reply.contains("created") || reply.at("created") != true) {
      throw std::runtime_error(
          "The realm returned an invalid registration response. Try signing in before retrying.");
    }
  } catch (const HttpError &error) {
    if (error.status() == 409) {
      throw std::runtime_error(
          "That account name is already taken. Choose another name or sign in.");
    }
    if (error.status() == 429) {
      throw std::runtime_error("Too many account creation attempts. Wait a minute and try again.");
    }
    if (error.status() == 400 || error.status() == 413) {
      throw std::runtime_error(
          "Use 3–32 letters, digits or underscores and a password of 8–256 bytes.");
    }
    if (error.status() == 401 || error.status() == 403 || error.status() == 404 ||
        error.status() == 405) {
      throw std::runtime_error("Account creation is unavailable on this realm. Contact support.");
    }
    throw std::runtime_error(
        "Could not confirm account creation. Try signing in before creating the account again.");
  } catch (const json::exception &) {
    throw std::runtime_error(
        "The realm returned an invalid registration response. Try signing in before retrying.");
  }
  // A previous failed/expired login must not remain as an error beside creation success.
  clear();
  return account;
}

void AccountSession::save_login(bool keep_signed_in, bool remember_password,
                                const std::string &password) {
  SavedLogin saved;
  saved.origin = origin_;
  saved.account = account_;
  saved.keep_signed_in = keep_signed_in;
  if (keep_signed_in) {
    saved.token = token_;
  }
  if (remember_password) {
    if (!password.empty()) {
      saved.password = password;
    } else {
      auto previous = credential_store_.load(origin_, account_);
      if (!previous || previous->password.empty()) {
        throw std::runtime_error("Choose Remember password when signing in to save your password.");
      }
      saved.password = previous->password;
    }
  }

  credential_store_.save(saved);
  keep_signed_in_ = keep_signed_in;
  remember_password_ = remember_password;
  saved_origin_ = origin_;
  saved_account_ = account_;
  persistence_error_.clear();
}

void AccountSession::set_login_preferences(bool keep_signed_in, bool remember_password) {
  if (token_.empty()) {
    throw std::runtime_error("Sign in before changing saved login settings.");
  }
  save_login(keep_signed_in, remember_password);
}

void AccountSession::forget_saved_login() {
  const auto &credential_origin = saved_origin_.empty() ? origin_ : saved_origin_;
  credential_store_.erase(credential_origin);
  keep_signed_in_ = false;
  remember_password_ = false;
  auto_resume_pending_ = false;
  saved_origin_.clear();
  saved_account_.clear();
  persistence_error_.clear();
}

void AccountSession::restore_saved_login(const std::string &origin, const std::string &account,
                                         bool validate_immediately) {
  // Credential Manager contains the saved account; local settings may have been removed.
  (void)account;
  clear_realm();
  try {
    saved_origin_ = distribution::parse_service_origin(origin).url;
    auto saved = credential_store_.load(saved_origin_, {});
    if (!saved) {
      return;
    }
    saved_account_ = normalize_account(saved->account);
    keep_signed_in_ = saved->keep_signed_in;
    remember_password_ = !saved->password.empty();
    if (!keep_signed_in_) {
      return;
    }

    origin_ = saved_origin_;
    account_ = saved_account_;
    token_ = std::move(saved->token);
    status_ = "signed_in";
    character_status_ = "loading";
    if (token_.empty()) {
      auto_resume_pending_ = remember_password_;
    }
    if (validate_immediately) {
      refresh(true);
    }
  } catch (const std::exception &error) {
    // Preserve the attempted realm so a damaged credential can still be forgotten.
    const auto failed_origin = saved_origin_;
    clear_realm();
    saved_origin_ = failed_origin;
    persistence_error_ = error.what();
  }
}

void AccountSession::resume_with_saved_password() {
  auto_resume_pending_ = false;
  try {
    sign_in(saved_origin_, saved_account_, {}, true, true, true);
  } catch (const std::exception &) {
    if (authentication_rejected_) {
      try {
        forget_saved_login();
      } catch (const std::exception &error) {
        persistence_error_ = error.what();
      }
    } else {
      auto_resume_pending_ = true;
      next_validation_ = now_() + kValidationInterval;
    }
  }
}

void AccountSession::sign_out() {
  // Erase first: a failed deletion must never report that persistent sign-in was disabled.
  forget_saved_login();
  ++attempt_;
  std::string warning;
  if (!token_.empty()) {
    try {
      transport_("POST", origin_ + xorstr_("/emulator/v1/logout"), json::object(), token_);
    } catch (const HttpError &error) {
      if (error.status() != 401 && error.status() != 403) {
        warning = "Signed out on this device. The realm could not confirm session revocation.";
      }
    } catch (const std::exception &) {
      warning = "Signed out on this device. The realm could not confirm session revocation.";
    }
  }
  clear();
  error_ = std::move(warning);
}

void AccountSession::expire() {
  clear();
  status_ = "expired";
  error_ = "Your session has ended. Sign in again to continue.";
  auto_resume_pending_ = keep_signed_in_ && remember_password_;
}

void AccountSession::refresh(bool force) {
  if (token_.empty() && auto_resume_pending_ && (force || now_() >= next_validation_)) {
    resume_with_saved_password();
    return;
  }
  if (token_.empty() || (!force && now_() < next_validation_)) {
    return;
  }
  next_validation_ = now_() + kValidationInterval;
  try {
    transport_("GET", origin_ + xorstr_("/emulator/v1/worlds"), json::object(), token_);
    connected_ = true;
    error_.clear();
  } catch (const HttpError &error) {
    if (error.status() == 401 || error.status() == 403) {
      expire();
      if (auto_resume_pending_) {
        resume_with_saved_password();
      }
      return;
    }
    connected_ = false;
    error_ = "Realm connection lost. Your session will be checked when it reconnects.";
  } catch (const std::exception &) {
    connected_ = false;
    error_ = "Realm connection lost. Your session will be checked when it reconnects.";
  }
}

bool AccountSession::matches(const std::string &origin, const std::string &account) const {
  return !token_.empty() && origin == origin_ && normalize_account(account) == account_;
}

std::string AccountSession::launch_url() {
  refresh(true);
  if (token_.empty()) {
    throw std::runtime_error("Sign in to your account before launching the game.");
  }
  if (!connected_) {
    throw std::runtime_error("The realm is unavailable. Reconnect before launching the game.");
  }
  try {
    auto reply =
        transport_("POST", origin_ + xorstr_("/emulator/v1/launch-grant"), json::object(), token_);
    auto url = reply.at("bootstrap_url").get<std::string>();
    const auto prefix = origin_ + "/launch/";
    if (!url.starts_with(prefix) || !url.ends_with('/') || url.size() != prefix.size() + 65 ||
        !distribution::valid_sha256(std::string_view(url).substr(prefix.size(), 64))) {
      throw std::runtime_error("The realm returned an invalid launch grant.");
    }
    return url;
  } catch (const HttpError &error) {
    if (error.status() == 401 || error.status() == 403) {
      expire();
      throw std::runtime_error(error_);
    }
    throw std::runtime_error("The realm could not prepare your game session. Please try again.");
  }
}

json AccountSession::state() const {
  return {{"status", status_},
          {"minimum_password_bytes", kMinimumPasswordBytes},
          {"maximum_password_bytes", kMaximumPasswordBytes},
          {"discord", {{"enabled", discord_enabled_},
                        {"pending", !discord_request_id_.empty()},
                        {"error", discord_error_}, {"result", discord_result_}}},
          {"authenticated", !token_.empty()},
          {"account", account_},
          {"connected", connected_},
          {"attempt", attempt_},
          {"error", error_},
          {"login_preferences",
           {{"keep_signed_in", keep_signed_in_},
            {"remember_password", remember_password_},
            {"saved_password", remember_password_},
            {"account", saved_account_},
            {"error", persistence_error_}}},
          {"characters",
           {{"status", character_status_}, {"error", character_error_}, {"entries", characters_}}}};
}

} // namespace chronicles::launcher
