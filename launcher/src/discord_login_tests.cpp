#include "account_session.hpp"

#include <boost/asio.hpp>
#include <gtest/gtest.h>
#include <windows.h>

#include <filesystem>

namespace chronicles::launcher {
namespace {

using Json = nlohmann::json;
const std::string kOrigin = "https://realm.example";
const std::string kRequestId(64, 'b');
const std::string kPollSecret(64, 'c');
const std::string kSessionToken(64, 'a');
const std::string kBrowserProof(64, 'd');

class LauncherDiscordTest : public testing::Test {
protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("chronicles-discord-test-" + std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(GetTickCount64()));
    ASSERT_TRUE(std::filesystem::create_directory(root_));
    target_prefix_ = L"Project Chronicles Discord Test/" + root_.filename().wstring() + L"/";
  }

  void TearDown() override {
    CredentialStore(root_ / "login.dat", target_prefix_).erase(kOrigin);
    const auto resolved = std::filesystem::canonical(root_);
    ASSERT_EQ(resolved.parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    ASSERT_TRUE(resolved.filename().string().starts_with("chronicles-discord-test-"));
    std::filesystem::remove_all(resolved);
  }

  Json request(const std::string &method, const std::string &url, const Json &body,
               const std::string &token) {
    ++requests_;
    if (url.ends_with("/auth-options")) {
      return {{"discord", true}};
    }
    if (url.ends_with("/discord/start")) {
      EXPECT_EQ(method, "POST");
      linked_ = body.at("link").get<bool>();
      port_ = body.at("loopback_port").get<unsigned short>();
      EXPECT_GE(port_, 1024);
      EXPECT_EQ(token, linked_ ? kSessionToken : std::string{});
      return {{"authorization_url", authorization_url_},
              {"request_id", kRequestId},
              {"poll_secret", kPollSecret}};
    }
    if (url.ends_with("/discord/poll")) {
      EXPECT_EQ(body.at("request_id"), kRequestId);
      EXPECT_EQ(body.at("poll_secret"), kPollSecret);
      EXPECT_EQ(body.at("browser_proof"), kBrowserProof);
      EXPECT_TRUE(token.empty());
      if (!completed_) {
        return {{"status", "pending"}};
      }
      if (linked_) {
        return {{"status", "linked"}};
      }
      return {
          {"status", "signed_in"}, {"account", "discord_test"}, {"access_token", kSessionToken}};
    }
    if (url.ends_with("/login")) {
      return {{"access_token", kSessionToken}};
    }
    return Json::object();
  }

  std::unique_ptr<AccountSession> session() {
    return std::make_unique<AccountSession>(
        [&](const auto &method, const auto &url, const auto &body, const auto &token) {
          return request(method, url, body, token);
        },
        [&] { return now_; }, root_ / "login.dat", target_prefix_);
  }

  void browser_callback(AccountSession &login, const std::string &state = kRequestId) {
    boost::asio::io_context context;
    boost::asio::ip::tcp::socket browser(context);
    browser.connect({boost::asio::ip::address_v4::loopback(), port_});
    const auto request = "GET /discord-complete?state=" + state + "&proof=" + kBrowserProof +
                         " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port_) +
                         "\r\nConnection: close\r\n\r\n";
    boost::asio::write(browser, boost::asio::buffer(request));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      login.tick_discord_login(true);
      if (!login.state().at("discord").at("pending").get<bool>()) {
        break;
      }
      Sleep(1);
    }
  }

  std::filesystem::path root_;
  std::wstring target_prefix_;
  AccountSession::TimePoint now_ = std::chrono::steady_clock::now();
  std::string authorization_url_ =
      "https://discord.com/oauth2/authorize?client_id=123456789012345678";
  unsigned requests_{};
  bool completed_{};
  bool linked_{};
  unsigned short port_{};
};

TEST_F(LauncherDiscordTest, PrivatePollingCompletesAndOnlyLauncherSessionIsRemembered) {
  auto login = session();
  login->refresh_discord_options(kOrigin);
  EXPECT_EQ(login->begin_discord_login(kOrigin, true), authorization_url_);
  EXPECT_TRUE(login->state().at("discord").at("pending"));
  EXPECT_EQ(login->state().dump().find(kPollSecret), std::string::npos);
  EXPECT_EQ(login->state().dump().find(kRequestId), std::string::npos);
  login->tick_discord_login(true);
  EXPECT_FALSE(login->state().at("authenticated"));
  completed_ = true;
  now_ += std::chrono::seconds(2);
  browser_callback(*login);
  EXPECT_TRUE(login->state().at("authenticated"));
  EXPECT_EQ(login->state().at("account"), "discord_test");
  EXPECT_EQ(login->state().dump().find(kSessionToken), std::string::npos);
  EXPECT_EQ(login->state().dump().find(kBrowserProof), std::string::npos);
  EXPECT_FALSE(login->state().at("login_preferences").at("saved_password"));

  auto restored = session();
  restored->restore_saved_login(kOrigin, "discord_test", false);
  EXPECT_TRUE(restored->state().at("authenticated"));
  EXPECT_TRUE(restored->state().at("login_preferences").at("keep_signed_in"));
}

TEST_F(LauncherDiscordTest, OfflineTimeoutAndCancelDoNotKeepPollingOrReplaceTheAccount) {
  auto login = session();
  login->sign_in(kOrigin, "existing", "12345678");
  login->refresh_discord_options(kOrigin);
  login->begin_discord_login(kOrigin, false);
  EXPECT_TRUE(linked_);
  const auto requests = requests_;
  now_ += std::chrono::minutes(5);
  login->tick_discord_login(false);
  EXPECT_EQ(requests_, requests);
  EXPECT_FALSE(login->state().at("discord").at("pending"));
  EXPECT_TRUE(login->state().at("authenticated"));
  EXPECT_EQ(login->state().at("account"), "existing");
  EXPECT_FALSE(login->state().at("discord").at("error").get<std::string>().empty());
  login->begin_discord_login(kOrigin, false);
  login->cancel_discord_login();
  completed_ = true;
  login->tick_discord_login(true);
  EXPECT_EQ(login->state().at("account"), "existing");
}

TEST_F(LauncherDiscordTest, WrongBrowserStateCannotCompleteAndListenerAcceptsRetry) {
  auto login = session();
  login->refresh_discord_options(kOrigin);
  login->begin_discord_login(kOrigin, false);
  completed_ = true;
  const auto requests = requests_;
  browser_callback(*login, std::string(64, 'f'));
  EXPECT_EQ(requests_, requests);
  EXPECT_FALSE(login->state().at("authenticated"));
  EXPECT_TRUE(login->state().at("discord").at("pending"));
  browser_callback(*login);
  EXPECT_TRUE(login->state().at("authenticated"));
}

TEST_F(LauncherDiscordTest, LinkingKeepsExistingAccountAndSession) {
  auto login = session();
  login->sign_in(kOrigin, "existing", "12345678");
  login->refresh_discord_options(kOrigin);
  login->begin_discord_login(kOrigin, false);
  completed_ = true;
  browser_callback(*login);
  EXPECT_TRUE(linked_);
  EXPECT_TRUE(login->state().at("authenticated"));
  EXPECT_EQ(login->state().at("account"), "existing");
  EXPECT_EQ(login->state().at("discord").at("result"), "linked");
}

TEST_F(LauncherDiscordTest, RejectsUntrustedBrowserUrlAndAnotherRealm) {
  auto login = session();
  login->refresh_discord_options(kOrigin);
  authorization_url_ = "https://untrusted.example/authorize";
  EXPECT_THROW(login->begin_discord_login(kOrigin, false), std::runtime_error);
  EXPECT_FALSE(login->state().at("discord").at("pending"));
  EXPECT_THROW(login->begin_discord_login("https://other-realm.example", false),
               std::runtime_error);
}

TEST(LauncherPasswordPolicy, EightBytesRegistersAndSevenBytesIsRejectedBeforeSending) {
  unsigned requests{};
  AccountSession login([&](const auto &, const auto &, const auto &body, const auto &) {
    ++requests;
    EXPECT_EQ(body.at("password"), "12345678");
    return Json{{"created", true}};
  });
  EXPECT_THROW(login.register_account(kOrigin, "eight", "1234567"), std::invalid_argument);
  EXPECT_EQ(requests, 0);
  EXPECT_EQ(login.register_account(kOrigin, "eight", "12345678"), "eight");
  EXPECT_EQ(requests, 1);
}

} // namespace
} // namespace chronicles::launcher
