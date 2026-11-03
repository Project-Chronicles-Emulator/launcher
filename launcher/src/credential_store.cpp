#include "credential_store.hpp"

#include "distribution/distribution.hpp"
#include "encoding.hpp"

#include <windows.h>
#include <wincred.h>
#include <wincrypt.h>
#include <dpapi.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <stdexcept>

namespace chronicles::launcher {
namespace {

using json = nlohmann::json;
constexpr std::size_t kMaximumLegacyCredentialBytes = 16 * 1024;

struct ProtectedBuffer {
  DATA_BLOB data{};

  ~ProtectedBuffer() {
    if (data.pbData != nullptr) {
      SecureZeroMemory(data.pbData, data.cbData);
      LocalFree(data.pbData);
    }
  }
};

struct CredentialDeleter {
  void operator()(CREDENTIALW *credential) const {
    if (credential != nullptr) {
      if (credential->CredentialBlob != nullptr) {
        SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
      }
      CredFree(credential);
    }
  }
};

struct ClearText {
  std::string value;

  ~ClearText() {
    SecureZeroMemory(value.data(), value.size());
  }
};

struct ClearDocumentSecrets {
  json &document;

  ~ClearDocumentSecrets() {
    for (const auto *key : {"password", "token"}) {
      auto value = document.find(key);
      if (value != document.end() && value->is_string()) {
        auto &secret = value->get_ref<std::string &>();
        SecureZeroMemory(secret.data(), secret.size());
      }
    }
  }
};

void validate_login(const SavedLogin &login) {
  if (login.origin.size() > 2048 ||
      distribution::parse_service_origin(login.origin).url != login.origin ||
      login.account.size() < 3 || login.account.size() > 32 || login.password.size() > 256 ||
      (!login.token.empty() && !distribution::valid_sha256(login.token)) ||
      (!login.keep_signed_in && !login.token.empty())) {
    throw std::runtime_error("Saved login has an invalid format. Forget it and sign in again.");
  }
}

std::wstring credential_target(const std::wstring &prefix, const std::string &origin) {
  if (prefix.empty() || origin.empty() ||
      distribution::parse_service_origin(origin).url != origin) {
    throw std::runtime_error("Saved login storage is unavailable for this server.");
  }

  const auto target = prefix + widen(distribution::sha256_bytes(origin));
  if (target.size() > CRED_MAX_GENERIC_TARGET_NAME_LENGTH) {
    throw std::runtime_error("Saved login storage is unavailable.");
  }
  return target;
}

std::unique_ptr<SavedLogin> parse_login(const BYTE *bytes, std::size_t size,
                                       const std::string &origin, bool legacy) {
  auto document = json::parse(bytes, bytes + size, nullptr, false);
  ClearDocumentSecrets clear_document{document};
  if (!document.is_object() || !document.contains("version") || document.at("version") != 1) {
    throw std::runtime_error("Saved login has an unsupported format. Forget it and sign in again.");
  }

  auto result = std::make_unique<SavedLogin>();
  try {
    result->origin = legacy ? document.at("origin").get<std::string>() : origin;
    result->account = document.at("account").get<std::string>();
    result->token = document.at("token").get<std::string>();
    result->password = document.at("password").get<std::string>();
    result->keep_signed_in = document.at("keep_signed_in").get<bool>();
  } catch (const json::exception &) {
    throw std::runtime_error("Saved login has an invalid format. Forget it and sign in again.");
  }
  validate_login(*result);
  return result;
}

std::unique_ptr<SavedLogin> read_legacy_login(const std::filesystem::path &path,
                                              const std::string &origin) {
  if (path.empty() || !std::filesystem::exists(path)) {
    return nullptr;
  }

  auto encrypted = distribution::read_bounded_file(path, kMaximumLegacyCredentialBytes);
  DATA_BLOB input{static_cast<DWORD>(encrypted.size()), reinterpret_cast<BYTE *>(encrypted.data())};
  ProtectedBuffer plain;
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                          &plain.data)) {
    throw std::runtime_error("Windows could not unlock the old saved login. Sign in again.");
  }
  if (plain.data.cbData == 0 || plain.data.cbData > kMaximumLegacyCredentialBytes) {
    throw std::runtime_error("Saved login exceeds the supported size.");
  }
  auto login = parse_login(plain.data.pbData, plain.data.cbData, origin, true);
  if (login->origin != origin) {
    return nullptr;
  }
  return login;
}

void remove_legacy_login(const std::filesystem::path &path) {
  if (path.empty()) {
    return;
  }
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error) {
    throw std::runtime_error("Windows saved your login, but could not remove the old login file.");
  }
}

} // namespace

SavedLogin::~SavedLogin() {
  SecureZeroMemory(token.data(), token.size());
  SecureZeroMemory(password.data(), password.size());
}

CredentialStore::CredentialStore(std::filesystem::path legacy_path, std::wstring target_prefix)
    : legacy_path_(std::move(legacy_path)), target_prefix_(std::move(target_prefix)) {}

std::unique_ptr<SavedLogin> CredentialStore::load(const std::string &origin,
                                                  const std::string &account) const {
  if (legacy_path_.empty()) {
    return nullptr;
  }

  const auto target = credential_target(target_prefix_, origin);
  CREDENTIALW *raw_credential = nullptr;
  if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &raw_credential)) {
    std::unique_ptr<CREDENTIALW, CredentialDeleter> credential(raw_credential);
    if (credential->CredentialBlob == nullptr || credential->CredentialBlobSize == 0 ||
        credential->CredentialBlobSize > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
      throw std::runtime_error("Saved login has an invalid format. Forget it and sign in again.");
    }
    auto login = parse_login(credential->CredentialBlob, credential->CredentialBlobSize,
                             origin, false);
    if (!account.empty() && login->account != account) {
      return nullptr;
    }
    return login;
  }
  if (GetLastError() != ERROR_NOT_FOUND) {
    throw std::runtime_error("Windows could not read the saved login.");
  }

  auto legacy = read_legacy_login(legacy_path_, origin);
  if (!legacy || (!account.empty() && legacy->account != account)) {
    return nullptr;
  }

  save(*legacy);
  auto migrated = load(origin, legacy->account);
  if (!migrated || migrated->token != legacy->token || migrated->password != legacy->password ||
      migrated->keep_signed_in != legacy->keep_signed_in) {
    throw std::runtime_error("Windows could not verify the migrated saved login.");
  }
  remove_legacy_login(legacy_path_);
  return migrated;
}

void CredentialStore::save(const SavedLogin &login) const {
  if (!login.keep_signed_in && login.password.empty()) {
    erase(login.origin);
    return;
  }
  if (legacy_path_.empty()) {
    throw std::runtime_error("Saved login storage is unavailable.");
  }
  validate_login(login);
  const auto target = credential_target(target_prefix_, login.origin);

  json document{{"version", 1},
                {"account", login.account},
                {"token", login.token},
                {"password", login.password},
                {"keep_signed_in", login.keep_signed_in}};
  ClearDocumentSecrets clear_document{document};
  ClearText plain{document.dump()};
  if (plain.value.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
    throw std::runtime_error("Saved login exceeds the Windows credential size limit.");
  }

  auto account = widen(login.account);
  CREDENTIALW credential{};
  credential.Type = CRED_TYPE_GENERIC;
  credential.TargetName = const_cast<wchar_t *>(target.c_str());
  credential.UserName = account.data();
  credential.CredentialBlob = reinterpret_cast<BYTE *>(plain.value.data());
  credential.CredentialBlobSize = static_cast<DWORD>(plain.value.size());
  credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
  if (!CredWriteW(&credential, 0)) {
    throw std::runtime_error("Windows could not save the login in Credential Manager.");
  }
}

void CredentialStore::erase(const std::string &origin) const {
  if (legacy_path_.empty() || origin.empty()) {
    return;
  }
  const auto target = credential_target(target_prefix_, origin);
  if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND) {
    throw std::runtime_error("Windows could not remove the saved login.");
  }
  if (std::filesystem::exists(legacy_path_)) {
    try {
      auto legacy = read_legacy_login(legacy_path_, origin);
      if (legacy) {
        remove_legacy_login(legacy_path_);
      }
    } catch (const std::exception &) {
      // Forget must also clear a legacy file that can no longer be decrypted.
      remove_legacy_login(legacy_path_);
    }
  }
}

} // namespace chronicles::launcher
