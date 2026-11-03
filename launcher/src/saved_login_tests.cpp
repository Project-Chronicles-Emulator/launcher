#include "account_session.hpp"
#include "distribution/distribution.hpp"
#include "http_client.hpp"

#include <windows.h>
#include <wincred.h>
#include <wincrypt.h>

#include <gtest/gtest.h>

#include <fstream>

namespace chronicles::launcher {
namespace {

using json = nlohmann::json;
const std::string kOrigin = "https://realm.example";
const std::string kToken(64, 'c');
const std::string kPassword = "disposable-test-password";

class SavedLoginTest : public testing::Test {
protected:
  void SetUp() override {
    directory_ = std::filesystem::temp_directory_path() /
                 ("chronicles-saved-login-test-" + std::to_string(GetCurrentProcessId()) + "-" +
                  std::to_string(GetTickCount64()));
    ASSERT_TRUE(std::filesystem::create_directory(directory_));
    path_ = directory_ / "login.dat";
    target_prefix_ = L"Project Chronicles Test/" + directory_.filename().wstring() + L"/";
  }

  void TearDown() override {
    CredentialStore(path_, target_prefix_).erase(kOrigin);
    CredentialStore(path_, target_prefix_).erase("https://other.example");
    const auto resolved = std::filesystem::canonical(directory_);
    ASSERT_EQ(resolved.parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    ASSERT_TRUE(resolved.filename().string().starts_with("chronicles-saved-login-test-"));
    std::filesystem::remove_all(resolved);
  }

  std::unique_ptr<AccountSession> session() {
    return std::make_unique<AccountSession>(
        [&](const auto &method, const auto &url, const auto &body, const auto &token) {
          ++requests_;
          if (offline_) {
            throw HttpError("Disposable offline fixture");
          }
          if (url.ends_with("/login")) {
            ++logins_;
            EXPECT_EQ(method, "POST");
            EXPECT_EQ(body.at("password"), kPassword);
            if (reject_password_) {
              throw HttpError("Disposable rejected password", 401);
            }
            if (login_network_failure_) {
              throw HttpError("Disposable login connection failure");
            }
            valid_token_ = true;
            return json{{"access_token", kToken}};
          }
          EXPECT_EQ(token, kToken);
          if (url.ends_with("/logout")) {
            valid_token_ = false;
            return json::object();
          }
          if (!valid_token_) {
            throw HttpError("Disposable expired session", 401);
          }
          return json::object();
        },
        [&] { return now_; }, path_, target_prefix_);
  }

  std::filesystem::path directory_;
  std::filesystem::path path_;
  std::wstring target_prefix_;
  AccountSession::TimePoint now_{std::chrono::steady_clock::now()};
  unsigned requests_{};
  unsigned logins_{};
  bool offline_{};
  bool valid_token_{};
  bool reject_password_{};
  bool login_network_failure_{};
};

TEST_F(SavedLoginTest, WindowsProtectedLoginRestoresWithoutSendingThePasswordAgain) {
  auto first = session();
  first->sign_in(kOrigin, "Alice", kPassword, true, true);
  EXPECT_FALSE(std::filesystem::exists(path_));
  EXPECT_EQ(first->state().dump().find(kPassword), std::string::npos);
  EXPECT_EQ(first->state().dump().find(kToken), std::string::npos);
  first.reset();

  path_ = directory_ / "relocated" / "login.dat";
  auto restored = session();
  restored->restore_saved_login(kOrigin, "", false);
  EXPECT_TRUE(restored->state().at("authenticated"));
  EXPECT_EQ(restored->state().at("account"), "alice");
  EXPECT_EQ(logins_, 1);
  restored->sign_out();
  EXPECT_FALSE(std::filesystem::exists(path_));
  EXPECT_FALSE(restored->state().at("authenticated"));
}

TEST_F(SavedLoginTest, RememberPasswordDoesNotAutomaticallySignInOrExposeThePassword) {
  session()->sign_in(kOrigin, "alice", kPassword, false, true);
  auto restored = session();
  const auto before_restore = requests_;
  restored->restore_saved_login(kOrigin, "alice");
  EXPECT_EQ(requests_, before_restore);
  EXPECT_FALSE(restored->state().at("authenticated"));
  EXPECT_TRUE(restored->state().at("login_preferences").at("saved_password"));
  EXPECT_EQ(restored->state().dump().find(kPassword), std::string::npos);

  EXPECT_THROW(restored->sign_in(kOrigin, "bob", {}, false, true, true), std::invalid_argument);
  EXPECT_THROW(restored->sign_in("https://other.example", "alice", {}, false, true, true),
               std::invalid_argument);
  EXPECT_EQ(requests_, before_restore);
  restored->sign_in(kOrigin, "alice", {}, false, true, true);
  EXPECT_TRUE(restored->state().at("authenticated"));
}

TEST_F(SavedLoginTest, KeepSignedInAloneStoresOnlyTheSessionAndHonorsServerExpiry) {
  session()->sign_in(kOrigin, "alice", kPassword, true, false);
  auto stored = CredentialStore(path_, target_prefix_).load(kOrigin, "alice");
  ASSERT_NE(stored, nullptr);
  EXPECT_TRUE(stored->password.empty());
  EXPECT_EQ(stored->token, kToken);
  auto restored = session();
  restored->restore_saved_login(kOrigin, "alice");
  EXPECT_TRUE(restored->state().at("authenticated"));
  valid_token_ = false;
  restored->refresh(true);
  EXPECT_EQ(restored->state().at("status"), "expired");
  EXPECT_EQ(logins_, 1);
}

TEST_F(SavedLoginTest, BothOptionsRenewAnExpiredSessionAndStopAfterPasswordRejection) {
  session()->sign_in(kOrigin, "alice", kPassword, true, true);
  valid_token_ = false;
  auto restored = session();
  restored->restore_saved_login(kOrigin, "alice");
  EXPECT_TRUE(restored->state().at("connected"));
  EXPECT_EQ(logins_, 2);

  valid_token_ = false;
  reject_password_ = true;
  restored->refresh(true);
  EXPECT_FALSE(restored->state().at("authenticated"));
  EXPECT_EQ(CredentialStore(path_, target_prefix_).load(kOrigin, "alice"), nullptr);
  const auto attempts_after_rejection = logins_;
  now_ += std::chrono::hours(1);
  restored->refresh(true);
  EXPECT_EQ(logins_, attempts_after_rejection);
}

TEST_F(SavedLoginTest, OfflineRestoreRetainsTheSessionAndRenewalRetriesAreBounded) {
  session()->sign_in(kOrigin, "alice", kPassword, true, true);
  auto restored = session();
  offline_ = true;
  restored->restore_saved_login(kOrigin, "alice");
  EXPECT_TRUE(restored->state().at("authenticated"));
  EXPECT_FALSE(restored->state().at("connected"));
  EXPECT_NE(CredentialStore(path_, target_prefix_).load(kOrigin, "alice"), nullptr);

  offline_ = false;
  valid_token_ = false;
  login_network_failure_ = true;
  restored->refresh(true);
  EXPECT_FALSE(restored->state().at("authenticated"));
  const auto failed_attempts = logins_;
  restored->refresh();
  EXPECT_EQ(logins_, failed_attempts);
  now_ += std::chrono::seconds(31);
  login_network_failure_ = false;
  restored->refresh();
  EXPECT_TRUE(restored->state().at("authenticated"));
  EXPECT_EQ(logins_, failed_attempts + 1);
}

TEST_F(SavedLoginTest, RemovingSavedCredentialsPreservesTheActiveSessionAndPreventsRestoration) {
  auto active = session();
  active->sign_in(kOrigin, "alice", kPassword, true, true);
  active->set_login_preferences(true, false);
  EXPECT_TRUE(CredentialStore(path_, target_prefix_).load(kOrigin, "alice")->password.empty());
  active->forget_saved_login();
  EXPECT_TRUE(active->state().at("authenticated"));
  EXPECT_FALSE(active->state().at("login_preferences").at("keep_signed_in"));
  auto reopened = session();
  reopened->restore_saved_login(kOrigin, "alice");
  EXPECT_FALSE(reopened->state().at("authenticated"));
}

TEST_F(SavedLoginTest, CredentialsRemainSeparateForEachServer) {
  SavedLogin first;
  first.origin = kOrigin;
  first.account = "alice";
  first.token = kToken;
  first.keep_signed_in = true;

  SavedLogin second;
  second.origin = "https://other.example";
  second.account = "bob";
  second.token = std::string(64, 'd');
  second.keep_signed_in = true;

  CredentialStore store(path_, target_prefix_);
  store.save(first);
  store.save(second);
  ASSERT_NE(store.load(kOrigin, {}), nullptr);
  EXPECT_EQ(store.load(kOrigin, {})->account, "alice");
  EXPECT_EQ(store.load(second.origin, {})->account, "bob");

  store.erase(kOrigin);
  EXPECT_EQ(store.load(kOrigin, {}), nullptr);
  EXPECT_EQ(store.load(second.origin, {})->account, "bob");
}

TEST_F(SavedLoginTest, RealmTransitionForgetsMetadataWithoutErasingPreviousCredential) {
  SavedLogin previous;
  previous.origin = kOrigin;
  previous.account = "alice";
  previous.token = kToken;
  previous.keep_signed_in = true;
  CredentialStore store(path_, target_prefix_);
  store.save(previous);

  auto active = session();
  active->restore_saved_login(kOrigin, "", false);
  ASSERT_TRUE(active->state().at("authenticated"));
  active->clear_realm();
  EXPECT_FALSE(active->state().at("authenticated"));
  active->sign_out();
  ASSERT_NE(store.load(kOrigin, {}), nullptr);
  EXPECT_EQ(store.load(kOrigin, {})->account, "alice");
}

TEST_F(SavedLoginTest, FailedNewRealmRestoreCannotReusePreviousRealmMetadata) {
  SavedLogin previous;
  previous.origin = kOrigin;
  previous.account = "alice";
  previous.token = kToken;
  previous.keep_signed_in = true;
  CredentialStore store(path_, target_prefix_);
  store.save(previous);

  auto active = session();
  active->restore_saved_login(kOrigin, "", false);
  active->restore_saved_login("invalid origin", "", false);
  active->sign_out();
  ASSERT_NE(store.load(kOrigin, {}), nullptr);
}

TEST_F(SavedLoginTest, ModifiedOrOversizedFilesFailClosedAndCanBeForgotten) {
  {
    std::ofstream corrupted(path_, std::ios::binary | std::ios::trunc);
    corrupted << "invalid-protected-login";
  }
  auto restored = session();
  restored->restore_saved_login(kOrigin, "alice");
  EXPECT_FALSE(restored->state().at("authenticated"));
  EXPECT_FALSE(restored->state().at("login_preferences").at("error").get<std::string>().empty());
  restored->forget_saved_login();
  EXPECT_FALSE(std::filesystem::exists(path_));

  {
    std::ofstream oversized(path_, std::ios::binary);
    oversized << std::string(16385, 'x');
  }
  EXPECT_THROW(CredentialStore(path_, target_prefix_).load(kOrigin, "alice"), std::runtime_error);
}

TEST_F(SavedLoginTest, StorageFailureDoesNotClaimThatLoginWasRemembered) {
  const auto original_prefix = target_prefix_;
  target_prefix_ = std::wstring(CRED_MAX_GENERIC_TARGET_NAME_LENGTH, L'x');
  auto active = session();
  active->sign_in(kOrigin, "alice", kPassword, true, true);
  const auto state = active->state();
  EXPECT_TRUE(state.at("authenticated"));
  EXPECT_FALSE(state.at("login_preferences").at("keep_signed_in"));
  EXPECT_FALSE(state.at("login_preferences").at("error").get<std::string>().empty());
  target_prefix_ = original_prefix;
}

TEST_F(SavedLoginTest, MigratesLegacyProtectedLoginAndRemovesTheOldFile) {
  const auto document = json{{"version", 1},
                             {"origin", kOrigin},
                             {"account", "alice"},
                             {"token", kToken},
                             {"password", kPassword},
                             {"keep_signed_in", true}}.dump();
  DATA_BLOB input{static_cast<DWORD>(document.size()),
                  reinterpret_cast<BYTE *>(const_cast<char *>(document.data()))};
  DATA_BLOB encrypted{};
  ASSERT_TRUE(CryptProtectData(&input, L"Project Chronicles test", nullptr, nullptr, nullptr,
                               CRYPTPROTECT_UI_FORBIDDEN, &encrypted));
  {
    std::ofstream output(path_, std::ios::binary);
    output.write(reinterpret_cast<const char *>(encrypted.pbData), encrypted.cbData);
    ASSERT_TRUE(output.good());
  }
  LocalFree(encrypted.pbData);

  auto restored = session();
  restored->restore_saved_login(kOrigin, "", false);
  EXPECT_TRUE(restored->state().at("authenticated"));
  EXPECT_EQ(restored->state().at("account"), "alice");
  EXPECT_FALSE(std::filesystem::exists(path_));
  EXPECT_NE(CredentialStore(path_, target_prefix_).load(kOrigin, "alice"), nullptr);
}

} // namespace
} // namespace chronicles::launcher
