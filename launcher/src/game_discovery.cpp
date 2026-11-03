#include "game_discovery.hpp"

#include "distribution/distribution.hpp"
#include "encoding.hpp"
#include "game_process.hpp"
#include "patch_installer.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace chronicles::launcher {
namespace {

constexpr std::size_t kMaximumValveFileBytes = 1024 * 1024;

std::optional<std::filesystem::path> registry_steam_path() {
  std::array<wchar_t, 32768> buffer{};
  DWORD size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
  const auto result = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                                   RRF_RT_REG_SZ, nullptr, buffer.data(), &size);
  if (result != ERROR_SUCCESS || size < sizeof(wchar_t)) {
    return std::nullopt;
  }
  return std::filesystem::path(buffer.data());
}

std::vector<std::string> read_valve_values(const std::filesystem::path &path,
                                           std::string_view key) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    return {};
  }
  return valve_values(distribution::read_bounded_file(path, kMaximumValveFileBytes), key);
}

bool is_completed_managed_game(const std::filesystem::path &directory,
                               std::string_view channel) {
  try {
    require_regular_patch_path(directory);
    const auto marker_path = directory / ".chronicles-install.json";
    require_regular_patch_path(marker_path);
    require_regular_patch_path(expected_client_executable(directory));
    if (!std::filesystem::is_regular_file(expected_client_executable(directory)) ||
        !std::filesystem::is_regular_file(marker_path)) {
      return false;
    }

    const auto marker = nlohmann::json::parse(distribution::read_bounded_file(marker_path, 65536));
    return marker.value("application", std::string{}) == "chronicles" &&
           marker.value("channel", std::string{}) == channel &&
           marker.value("sequence", std::uint64_t{}) > 0 &&
           distribution::valid_sha256(marker.value("payload_sha256", std::string{}));
  } catch (const std::exception &) {
    // A broken or inaccessible candidate cannot safely be selected automatically.
    return false;
  }
}

bool is_game_directory_name(const std::filesystem::path &path) {
  const auto name = path.filename().wstring();
  return _wcsnicmp(name.c_str(), L"Project Chronicles", 18) == 0 ||
         _wcsnicmp(name.c_str(), L"Chronicles", 10) == 0 ||
         _wcsnicmp(name.c_str(), L"ArcheAge", 8) == 0;
}

bool is_unmanaged_or_incomplete_game(const std::filesystem::path &directory,
                                     std::string_view channel) {
  try {
    require_regular_patch_path(directory);
    const auto executable = expected_client_executable(directory);
    const auto marker = directory / ".chronicles-install.json";
    require_regular_patch_path(executable);
    require_regular_patch_path(marker);
    if (!std::filesystem::is_regular_file(executable)) {
      return false;
    }
    if (!std::filesystem::exists(marker)) {
      return true;
    }

    const auto document = nlohmann::json::parse(distribution::read_bounded_file(marker, 65536));
    return document.value("application", std::string{}) == "chronicles" &&
           document.value("channel", std::string{}) == channel &&
           document.value("sequence", std::uint64_t{}) == 0;
  } catch (const std::exception &) {
    return false;
  }
}

template <typename Predicate>
std::optional<std::filesystem::path> find_game_in_roots(
    const std::vector<std::filesystem::path> &search_roots, Predicate matches) {
  for (const auto &root : search_roots) {
    for (const auto &candidate : {root, root / "runtime/game", root / "game"}) {
      if (matches(candidate)) {
        return candidate;
      }
    }

    std::error_code error;
    std::vector<std::filesystem::path> children;
    for (std::filesystem::directory_iterator entry(root, error), end; !error && entry != end;
         entry.increment(error)) {
      if (is_game_directory_name(entry->path())) {
        children.push_back(entry->path());
      }
    }
    std::ranges::sort(children);
    for (const auto &child : children) {
      for (const auto &candidate : {child, child / "runtime/game", child / "game"}) {
        if (matches(candidate)) {
          return candidate;
        }
      }
    }
  }
  return std::nullopt;
}

} // namespace

std::vector<std::string> valve_values(std::string_view document, std::string_view key) {
  if (document.size() > kMaximumValveFileBytes) {
    throw std::invalid_argument("Steam library metadata is too large");
  }

  std::vector<std::string> values;
  std::optional<std::string> previous;
  for (std::size_t offset = 0; offset < document.size(); ++offset) {
    if (document[offset] == '{' || document[offset] == '}') {
      previous.reset();
    }
    if (document[offset] != '"') {
      continue;
    }

    std::string token;
    bool closed = false;
    while (++offset < document.size()) {
      const auto character = document[offset];
      if (character == '"') {
        closed = true;
        break;
      }
      if (character == '\\') {
        if (++offset >= document.size()) {
          break;
        }
        token += document[offset];
      } else {
        token += character;
      }
    }
    if (!closed) {
      throw std::invalid_argument("Steam library metadata has an unfinished string");
    }

    if (previous) {
      if (*previous == key) {
        values.push_back(token);
      }
      previous.reset();
    } else {
      previous = std::move(token);
    }
  }
  return values;
}

std::optional<std::filesystem::path> discover_existing_game() {
  std::vector<std::filesystem::path> roots;
  if (const auto registered = registry_steam_path()) {
    roots.push_back(*registered);
  }
  roots.emplace_back(L"C:/Program Files (x86)/Steam");
  roots.emplace_back(L"C:/Program Files/Steam");

  const auto primary_roots = roots;
  for (const auto &root : primary_roots) {
    for (const auto &library : read_valve_values(root / "steamapps/libraryfolders.vdf", "path")) {
      roots.emplace_back(widen(library));
    }
  }

  for (const auto &root : roots) {
    const auto steamapps = root / "steamapps";
    for (const auto &installed :
         read_valve_values(steamapps / "appmanifest_5280930.acf", "installdir")) {
      const auto relative = std::filesystem::path(widen(installed));
      if (relative.empty() || relative.has_parent_path() || relative.is_absolute()) {
        continue;
      }
      const auto candidate = steamapps / "common" / relative;
      std::error_code error;
      if (std::filesystem::is_regular_file(expected_client_executable(candidate), error)) {
        return candidate;
      }
    }
  }
  return std::nullopt;
}

std::optional<std::filesystem::path> find_managed_game(
    const std::vector<std::filesystem::path> &search_roots, std::string_view channel) {
  return find_game_in_roots(search_roots, [&](const auto &candidate) {
    return is_completed_managed_game(candidate, channel);
  });
}

std::optional<std::filesystem::path> find_other_game(
    const std::vector<std::filesystem::path> &search_roots, std::string_view channel) {
  if (const auto managed = find_managed_game(search_roots, channel)) {
    return managed;
  }
  return find_game_in_roots(search_roots, [&](const auto &candidate) {
    return is_unmanaged_or_incomplete_game(candidate, channel);
  });
}

std::optional<std::filesystem::path> discover_other_game(
    const std::filesystem::path &launcher_directory, std::string_view channel) {
  std::vector<std::filesystem::path> roots{launcher_directory};

  std::array<wchar_t, 32768> local_app_data{};
  const auto local_length = GetEnvironmentVariableW(
      L"LOCALAPPDATA", local_app_data.data(), static_cast<DWORD>(local_app_data.size()));
  if (local_length > 0 && local_length < local_app_data.size()) {
    roots.emplace_back(std::filesystem::path(local_app_data.data()) / "Programs/Chronicles");
  }

  std::array<wchar_t, 1024> drives{};
  const auto drive_length = GetLogicalDriveStringsW(static_cast<DWORD>(drives.size()), drives.data());
  if (drive_length > 0 && drive_length < drives.size()) {
    for (const wchar_t *drive = drives.data(); *drive != L'\0'; drive += wcslen(drive) + 1) {
      if (GetDriveTypeW(drive) == DRIVE_FIXED) {
        roots.emplace_back(drive);
      }
    }
  }

  return find_other_game(roots, channel);
}

} // namespace chronicles::launcher
