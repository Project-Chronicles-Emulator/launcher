#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace chronicles::launcher {

struct SavedLogin {
  std::string origin;
  std::string account;
  std::string token;
  std::string password;
  bool keep_signed_in{};

  SavedLogin() = default;
  ~SavedLogin();
  SavedLogin(const SavedLogin &) = delete;
  SavedLogin &operator=(const SavedLogin &) = delete;
};

// A generic Windows credential is scoped to the current user and API origin.
// The legacy path is read only for migration from older launcher versions.
class CredentialStore {
public:
  explicit CredentialStore(std::filesystem::path legacy_path,
                           std::wstring target_prefix = L"Project Chronicles Launcher/");

  std::unique_ptr<SavedLogin> load(const std::string &origin, const std::string &account) const;
  void save(const SavedLogin &login) const;
  void erase(const std::string &origin) const;

private:
  std::filesystem::path legacy_path_;
  std::wstring target_prefix_;
};

} // namespace chronicles::launcher
