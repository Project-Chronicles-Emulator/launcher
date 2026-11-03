#include "account_session.hpp"
#include "http_client.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

namespace chronicles::launcher {
namespace {

using json = nlohmann::json;
const std::string kOrigin = "https://realm.example";
const std::string kToken(64, 'a');
const std::string kGrant(64, 'b');

class AccountSessionTest : public testing::Test {
protected:
  json request(const std::string &method, const std::string &url, const json &body,
               const std::string &token) {
    ++requests_;
    if (failure_status_ != 0) {
      throw HttpError("Synthetic HTTP failure", failure_status_);
    }
    if (offline_) {
      throw HttpError("Synthetic transport failure");
    }
    if (url.ends_with("/login")) {
      EXPECT_EQ(method, "POST");
      EXPECT_EQ(body.at("client"), "launcher");
      EXPECT_EQ(body.at("account"), "alice");
      EXPECT_TRUE(token.empty());
      return invalid_password_ ? json::object() : json{{"access_token", kToken}};
    }
    EXPECT_EQ(token, kToken);
    if (url.ends_with("/launch-grant")) {
      return {{"bootstrap_url", grant_origin_ + "/launch/" + kGrant + "/"}};
    }
    return json::object();
  }

  AccountSession session_{[&](const auto &method, const auto &url, const auto &body,
                              const auto &token) { return request(method, url, body, token); },
                          [&] { return now_; }};
  AccountSession::TimePoint now_{std::chrono::steady_clock::now()};
  std::string grant_origin_{kOrigin};
  unsigned failure_status_{};
  unsigned requests_{};
  bool offline_{};
  bool invalid_password_{};
};

TEST_F(AccountSessionTest, OnlySuccessfulLoginPublishesAnAccountAndNeverPublishesCredentials) {
  EXPECT_EQ(session_.state().at("status"), "signed_out");
  EXPECT_FALSE(session_.state().at("authenticated"));
  invalid_password_ = true;
  EXPECT_THROW(session_.sign_in(kOrigin, "Alice", "wrong-password"), std::runtime_error);
  EXPECT_EQ(session_.state().at("status"), "error");
  EXPECT_EQ(session_.state().at("account"), "");

  invalid_password_ = false;
  session_.sign_in(kOrigin, "Alice", "private-test-password");
  EXPECT_TRUE(session_.matches(kOrigin, "ALICE"));
  EXPECT_FALSE(session_.matches("https://other.example", "alice"));
  EXPECT_FALSE(session_.matches(kOrigin, "bob"));
  EXPECT_EQ(session_.state().at("account"), "alice");
  EXPECT_EQ(session_.state().at("attempt"), 2);
  EXPECT_EQ(session_.state().dump().find(kToken), std::string::npos);
  EXPECT_EQ(session_.state().dump().find("private-test-password"), std::string::npos);
}

TEST_F(AccountSessionTest, InvalidInputNeverSendsCredentialsAndRateLimitsAreActionable) {
  EXPECT_THROW(session_.sign_in("http://remote.example", "alice", "password"),
               std::invalid_argument);
  EXPECT_THROW(session_.sign_in(kOrigin, "bad/account", "password"), std::invalid_argument);
  EXPECT_EQ(requests_, 0);
  failure_status_ = 429;
  EXPECT_THROW(session_.sign_in(kOrigin, "alice", "password"), std::runtime_error);
  EXPECT_NE(session_.state().at("error").get<std::string>().find("Wait"), std::string::npos);
  EXPECT_FALSE(session_.state().at("authenticated"));
}

TEST_F(AccountSessionTest, ConnectionLossRetainsIdentityButRevocationExpiresIt) {
  session_.sign_in(kOrigin, "alice", "password");
  session_.refresh();
  EXPECT_EQ(requests_, 1);
  now_ += std::chrono::seconds(31);
  offline_ = true;
  session_.refresh();
  EXPECT_TRUE(session_.state().at("authenticated"));
  EXPECT_FALSE(session_.state().at("connected"));
  EXPECT_THROW(session_.launch_url(), std::runtime_error);

  offline_ = false;
  session_.refresh(true);
  EXPECT_TRUE(session_.state().at("connected"));
  failure_status_ = 401;
  session_.refresh(true);
  EXPECT_EQ(session_.state().at("status"), "expired");
  EXPECT_FALSE(session_.state().at("authenticated"));
  EXPECT_EQ(session_.state().at("account"), "");
}

TEST_F(AccountSessionTest, LaunchReusesTheSessionAndRejectsACrossOriginGrant) {
  session_.sign_in(kOrigin, "alice", "password");
  EXPECT_EQ(session_.launch_url(), kOrigin + "/launch/" + kGrant + "/");
  EXPECT_EQ(requests_, 3);
  EXPECT_EQ(session_.state().at("attempt"), 1);
  grant_origin_ = "https://untrusted.example";
  EXPECT_THROW(session_.launch_url(), std::runtime_error);
  EXPECT_EQ(session_.state().dump().find(kGrant), std::string::npos);
}

TEST_F(AccountSessionTest, OfflineSignOutClearsTheNativeSessionAndStopsAutomaticValidation) {
  session_.sign_in(kOrigin, "alice", "password");
  offline_ = true;
  session_.sign_out();
  EXPECT_EQ(session_.state().at("status"), "signed_out");
  EXPECT_FALSE(session_.state().at("authenticated"));
  EXPECT_FALSE(session_.state().at("error").get<std::string>().empty());
  const auto previous_requests = requests_;
  now_ += std::chrono::hours(1);
  session_.refresh(true);
  EXPECT_EQ(requests_, previous_requests);
  EXPECT_FALSE(session_.matches(kOrigin, "alice"));
}

TEST(AccountRegistrationTest, CreatesCanonicalAccountWithoutSigningInOrPublishingCredentials) {
  unsigned requests{};
  AccountSession session(
      [&](const auto &method, const auto &url, const auto &body, const auto &token) {
        ++requests;
        EXPECT_EQ(method, "POST");
        EXPECT_EQ(url, kOrigin + "/emulator/v1/register");
        EXPECT_EQ(body.at("account"), "new_player");
        EXPECT_EQ(body.at("password"), "synthetic-registration-password");
        EXPECT_TRUE(token.empty());
        return json{{"created", true}};
      });
  const auto before = session.state();
  EXPECT_EQ(session.register_account(kOrigin, "New_Player", "synthetic-registration-password"),
            "new_player");
  EXPECT_EQ(requests, 1);
  EXPECT_EQ(session.state(), before);
  EXPECT_FALSE(session.state().at("authenticated"));
}

TEST(AccountRegistrationTest, InvalidInputAndRemoteHttpNeverSendCredentials) {
  unsigned requests{};
  AccountSession session([&](const auto &, const auto &, const auto &, const auto &) {
    ++requests;
    return json{{"created", true}};
  });
  EXPECT_THROW(session.register_account(kOrigin, "ab", "synthetic-registration-password"),
               std::invalid_argument);
  EXPECT_THROW(session.register_account(kOrigin, "new/player", "synthetic-registration-password"),
               std::invalid_argument);
  EXPECT_THROW(session.register_account(kOrigin, "new_player", "short"), std::invalid_argument);
  EXPECT_THROW(session.register_account(kOrigin, "new_player", std::string(257, 'x')),
               std::invalid_argument);
  EXPECT_THROW(session.register_account("http://remote.example", "new_player",
                                        "synthetic-registration-password"),
               std::invalid_argument);
  EXPECT_EQ(requests, 0);
}

TEST(AccountRegistrationTest, HttpFailuresAreActionableAndNeverQuoteSecretPayloads) {
  for (const auto status : {400U, 401U, 403U, 404U, 409U, 429U, 503U}) {
    AccountSession session(
        [status](const auto &, const auto &, const auto &, const auto &) -> json {
          throw HttpError("untrusted-error-containing-a-secret", status);
        });
    try {
      session.register_account(kOrigin, "new_player", "synthetic-registration-password");
      FAIL() << "Expected registration failure";
    } catch (const std::runtime_error &error) {
      const std::string message = error.what();
      EXPECT_EQ(message.find("untrusted-error-containing-a-secret"), std::string::npos);
      EXPECT_EQ(message.find("synthetic-registration-password"), std::string::npos);
      EXPECT_FALSE(message.empty());
    }
    EXPECT_FALSE(session.state().at("authenticated"));
  }
}

TEST(AccountRegistrationTest, UnexpectedReplyDoesNotClaimSuccessOrCreateALogin) {
  AccountSession session([](const auto &, const auto &, const auto &, const auto &) {
    return json{{"created", false}};
  });
  EXPECT_THROW(session.register_account(kOrigin, "new_player", "synthetic-registration-password"),
               std::runtime_error);
  EXPECT_FALSE(session.state().at("authenticated"));
}

TEST_F(AccountSessionTest, RegistrationCannotReplaceAnActiveAccountSession) {
  session_.sign_in(kOrigin, "alice", "password");
  const auto before = session_.state();
  const auto before_requests = requests_;
  EXPECT_THROW(session_.register_account(kOrigin, "new_player", "synthetic-registration-password"),
               std::runtime_error);
  EXPECT_EQ(session_.state(), before);
  EXPECT_EQ(requests_, before_requests);
}

TEST(AccountRegistrationTest, MalformedResponseCannotExposeCredentialTextInErrors) {
  AccountSession session([](const auto &, const auto &, const auto &, const auto &) {
    return json::parse("{\"echo\": \"synthetic-registration-password");
  });
  try {
    session.register_account(kOrigin, "new_player", "synthetic-registration-password");
    FAIL() << "Expected invalid registration response";
  } catch (const std::runtime_error &error) {
    EXPECT_EQ(std::string(error.what()).find("synthetic-registration-password"), std::string::npos);
  }
}

TEST(AccountRegistrationTest, SuccessfulCreationClearsAnEarlierLoginFailure) {
  AccountSession session([](const auto &, const auto &url, const auto &, const auto &) {
    return url.ends_with("/register") ? json{{"created", true}} : json::object();
  });
  EXPECT_THROW(session.sign_in(kOrigin, "new_player", "synthetic-registration-password"),
               std::runtime_error);
  EXPECT_FALSE(session.state().at("error").get<std::string>().empty());
  session.register_account(kOrigin, "new_player", "synthetic-registration-password");
  EXPECT_EQ(session.state().at("status"), "signed_out");
  EXPECT_EQ(session.state().at("error"), "");
  EXPECT_FALSE(session.state().at("authenticated"));
}

TEST(AccountRegistrationIntegration, RealBackendCreatesAccountThatCanSignIn) {
  const auto *origin = std::getenv("CHRONICLES_REGISTRATION_TEST_API");
  if (origin == nullptr) {
    GTEST_SKIP() << "Run tools/registration_smoke_test.py for the disposable backend fixture";
  }
  AccountSession session;
  EXPECT_EQ(session.register_account(origin, "Launcher_Signup", "synthetic-registration-password"),
            "launcher_signup");
  EXPECT_FALSE(session.state().at("authenticated"));
  EXPECT_THROW(session.register_account(origin, "launcher_signup", "synthetic-different-password"),
               std::runtime_error);
  session.sign_in(origin, "launcher_signup", "synthetic-registration-password");
  EXPECT_TRUE(session.state().at("connected"));
  session.sign_out();
}

TEST(AccountSessionIntegration, RealBackendAcceptsLoginReuseAndNoContentLogout) {
  const auto *origin = std::getenv("CHRONICLES_LAUNCHER_TEST_API");
  if (origin == nullptr) {
    GTEST_SKIP() << "Run through distribution_smoke_test.py for the disposable backend fixture";
  }
  AccountSession session;
  EXPECT_THROW(session.sign_in(origin, "distribution_fixture", "wrong-password"),
               std::runtime_error);
  EXPECT_FALSE(session.state().at("authenticated"));
  session.sign_in(origin, "distribution_fixture", "synthetic-distribution-password-123");
  ASSERT_TRUE(session.state().at("authenticated"));
  EXPECT_TRUE(session.launch_url().starts_with(std::string(origin) + "/launch/"));
  session.refresh(true);
  EXPECT_TRUE(session.state().at("connected"));
  session.sign_out();
  EXPECT_FALSE(session.state().at("authenticated"));
  EXPECT_EQ(session.state().at("error"), "");
}

} // namespace
} // namespace chronicles::launcher
