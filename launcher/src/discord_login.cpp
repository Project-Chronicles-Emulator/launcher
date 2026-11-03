#include "account_session.hpp"

#include "discord_loopback.hpp"
#include "distribution/distribution.hpp"
#include "xorstr.hpp"

#include <windows.h>

#include <algorithm>
#include <stdexcept>

namespace chronicles::launcher {

void AccountSession::refresh_discord_options(const std::string &origin) {
  const auto normalized = distribution::parse_service_origin(origin).url;
  if (normalized != discord_origin_) {
    cancel_discord_login();
    discord_origin_ = normalized;
    discord_enabled_ = false;
    discord_next_options_ = {};
  }
  if (now_() < discord_next_options_) {
    return;
  }
  discord_next_options_ = now_() + std::chrono::seconds(30);
  try {
    const auto options =
        transport_("GET", normalized + xorstr_("/emulator/v1/auth-options"), {}, {});
    discord_enabled_ = options.value("discord", false);
  } catch (const std::exception &) {
    discord_enabled_ = false;
  }
}

std::string AccountSession::begin_discord_login(const std::string &origin, bool keep_signed_in) {
  const auto normalized = distribution::parse_service_origin(origin).url;
  cancel_discord_login();
  discord_error_.clear();
  discord_result_.clear();
  if (discord_origin_ != normalized || !discord_enabled_) {
    throw std::runtime_error("Discord sign-in is not enabled on this server.");
  }
  try {
    const bool linking = !token_.empty() && origin_ == normalized;
    discord_loopback_ = std::make_unique<DiscordLoopback>();
    const auto reply = transport_("POST", normalized + xorstr_("/emulator/v1/discord/start"),
                                  {{"link", linking}, {"loopback_port", discord_loopback_->port()}},
                                  linking ? token_ : std::string{});
    const auto url = reply.at("authorization_url").get<std::string>();
    const auto request_id = reply.at("request_id").get<std::string>();
    const auto poll_secret = reply.at("poll_secret").get<std::string>();
    if (!url.starts_with("https://discord.com/oauth2/authorize?") || url.size() > 8192 ||
        !distribution::valid_sha256(request_id) || !distribution::valid_sha256(poll_secret)) {
      throw std::runtime_error("Invalid Discord sign-in response");
    }
    discord_request_id_ = request_id;
    discord_loopback_->expect_state(request_id);
    discord_poll_secret_ = poll_secret;
    discord_deadline_ = now_() + std::chrono::minutes(5);
    discord_next_poll_ = {};
    discord_keep_signed_in_ = keep_signed_in;
    discord_linking_ = linking;
    return url;
  } catch (const std::exception &) {
    cancel_discord_login();
    discord_error_ = "Discord sign-in could not start. Please try again.";
    throw std::runtime_error(discord_error_);
  }
}

void AccountSession::cancel_discord_login() {
  discord_loopback_.reset();
  if (!discord_browser_proof_.empty()) {
    SecureZeroMemory(discord_browser_proof_.data(), discord_browser_proof_.size());
    discord_browser_proof_.clear();
  }
  discord_request_id_.clear();
  if (!discord_poll_secret_.empty()) {
    SecureZeroMemory(discord_poll_secret_.data(), discord_poll_secret_.size());
    discord_poll_secret_.clear();
  }
}

void AccountSession::tick_discord_login(bool server_online) {
  if (discord_request_id_.empty()) {
    return;
  }
  if (now_() >= discord_deadline_) {
    cancel_discord_login();
    discord_error_ = "Discord sign-in expired. Please try again.";
    return;
  }
  if (discord_loopback_ && discord_browser_proof_.empty()) {
    discord_browser_proof_ = discord_loopback_->poll();
  }
  if (discord_browser_proof_.empty()) {
    return;
  }
  if (!server_online || now_() < discord_next_poll_) {
    return;
  }
  discord_next_poll_ = now_() + std::chrono::seconds(2);
  nlohmann::json reply;
  try {
    reply = transport_("POST", discord_origin_ + xorstr_("/emulator/v1/discord/poll"),
                       {{"request_id", discord_request_id_},
                        {"poll_secret", discord_poll_secret_},
                        {"browser_proof", discord_browser_proof_}},
                       {});
  } catch (const std::exception &) {
    // A transient network error must not discard an authorization still open in the browser.
    return;
  }
  try {
    const auto result = reply.at("status").get<std::string>();
    if (result == "pending") {
      return;
    }
    cancel_discord_login();
    if (result == "linked" && discord_linking_) {
      discord_result_ = "linked";
      return;
    }
    if (result != "signed_in" || discord_linking_) {
      throw std::runtime_error("Discord sign-in was not accepted");
    }
    const auto account = reply.at("account").get<std::string>();
    const auto token = reply.at("access_token").get<std::string>();
    if (account.size() < 3 || account.size() > 32 || !distribution::valid_sha256(token) ||
        !std::all_of(account.begin(), account.end(), [](char value) {
          return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_';
        })) {
      throw std::runtime_error("Invalid Project Chronicles Discord session");
    }
    origin_ = discord_origin_;
    account_ = account;
    token_ = token;
    status_ = "signed_in";
    error_.clear();
    connected_ = true;
    next_validation_ = now_() + std::chrono::seconds(30);
    character_status_ = "loading";
    discord_result_ = "signed_in";
    try {
      save_login(discord_keep_signed_in_, false);
    } catch (const std::exception &) {
      persistence_error_ = "Signed in, but Windows could not save this login.";
      keep_signed_in_ = false;
      remember_password_ = false;
    }
  } catch (const std::exception &) {
    cancel_discord_login();
    discord_error_ = "Discord sign-in could not complete. Please try again.";
  }
}

} // namespace chronicles::launcher
