#include "game_compatibility.hpp"

#include "patch_installer.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace chronicles::launcher {

std::vector<distribution::PatchFile>
legacy_game_runtime_files(const std::filesystem::path &launcher_directory) {
  const auto path = launcher_directory / "resources/legacy-game-runtime-files.json";
  require_regular_patch_path(path);
  const auto bytes = distribution::read_bounded_file(path, 65536);
  constexpr auto expected_hash = "79217a922eb3ca62f7cad30584bea63649c0bae1dc9f71459e98dd5dab5be528";
  if (distribution::sha256_bytes(bytes) != expected_hash) {
    throw std::runtime_error(
        "The Project Chronicles game compatibility receipt is damaged. Reinstall the launcher.");
  }

  const auto document = nlohmann::json::parse(bytes);
  std::vector<distribution::PatchFile> files;
  for (const auto &entry : document.at("files")) {
    files.push_back({entry.at("path").get<std::string>(), entry.at("sha256").get<std::string>(),
                     entry.at("size").get<std::uint64_t>()});
  }
  return files;
}

void exclude_legacy_game_runtimes(distribution::Release &release,
                                  std::span<const distribution::PatchFile> legacy_files) {
  if (release.version != "0.2.0" || release.sequence != 2) {
    return;
  }

  std::erase_if(release.files, [&](const auto &file) {
    const auto owned = std::ranges::find(legacy_files, file.path, &distribution::PatchFile::path);
    if (owned == legacy_files.end()) {
      return false;
    }
    if (file.sha256 != owned->sha256 || file.size != owned->size) {
      throw std::runtime_error("The legacy game runtime differs from its published receipt.");
    }
    release.total_bytes -= file.size;
    return true;
  });
}

void require_managed_steam_compatibility(const distribution::Release &release,
                                         const std::filesystem::path &game_directory) {
  constexpr std::string_view directory =
      "Engine/Binaries/ThirdParty/Steamworks/Steamv161/Win64/";
  constexpr std::array<std::string_view, 3> required_names = {
      "steam_api64.dll", "libprotobuf.dll", "abseil_dll.dll"};

  for (const auto name : required_names) {
    const auto relative = std::string(directory) + std::string(name);
    const auto signed_file = std::ranges::find(release.files, relative,
                                               &distribution::PatchFile::path);
    if (signed_file == release.files.end()) {
      throw std::runtime_error("The signed game release is missing Steam compatibility files.");
    }

    const auto installed_file = game_directory / relative;
    require_regular_patch_path(installed_file);
    if (!std::filesystem::is_regular_file(installed_file) ||
        std::filesystem::file_size(installed_file) != signed_file->size ||
        distribution::sha256_file(installed_file) != signed_file->sha256) {
      throw std::runtime_error(
          "Steam compatibility files are missing or changed. Use Settings > Verify / repair game files.");
    }
  }
}

std::filesystem::path
quarantine_managed_game_files(const std::filesystem::path &game_directory,
                              std::span<const distribution::PatchFile> owned_files) {
  require_regular_patch_path(game_directory);
  const auto root = std::filesystem::absolute(game_directory).lexically_normal();
  if (root == root.root_path() || root.wstring().starts_with(L"\\\\")) {
    throw std::runtime_error("Game preparation requires a dedicated local managed folder.");
  }

  std::vector<distribution::PatchFile> present;
  for (const auto &file : owned_files) {
    if (!distribution::valid_patch_path(file.path) || !distribution::valid_sha256(file.sha256)) {
      throw std::invalid_argument("Invalid game-file ownership receipt");
    }
    const auto source = root / file.path;
    require_regular_patch_path(source);
    if (!std::filesystem::exists(source)) {
      continue;
    }
    if (!std::filesystem::is_regular_file(source) ||
        std::filesystem::file_size(source) != file.size ||
        distribution::sha256_file(source) != file.sha256) {
      throw std::runtime_error("An unexpected game runtime was preserved: " + file.path +
                               ". Select a clean game folder or contact support.");
    }
    present.push_back(file);
  }
  if (present.empty()) {
    return {};
  }

  const auto marker_path = root / ".chronicles-install.json";
  require_regular_patch_path(marker_path);
  const auto marker = nlohmann::json::parse(distribution::read_bounded_file(marker_path, 65536));
  if (marker.value("application", std::string{}) != "chronicles") {
    throw std::runtime_error("Runtime preparation is limited to a managed Project Chronicles installation.");
  }

  const auto backup_parent =
      root.parent_path() / (root.filename().wstring() + L".chronicles-runtime-backups");
  const auto backup = backup_parent / ("legacy-020-" + std::to_string(GetCurrentProcessId()) + "-" +
                                       std::to_string(GetTickCount64()));
  require_regular_patch_path(backup);
  if (!std::filesystem::create_directories(backup)) {
    throw std::runtime_error("The game runtime backup directory already exists.");
  }
  for (const auto &file : present) {
    const auto source = root / file.path;
    const auto destination = backup / file.path;
    require_regular_patch_path(source);
    require_regular_patch_path(destination);
    if (distribution::sha256_file(source) != file.sha256) {
      throw std::runtime_error(
          "A game runtime changed during preparation. Nothing was overwritten.");
    }
    std::filesystem::create_directories(destination.parent_path());
    std::filesystem::rename(source, destination);
  }
  return backup;
}

} // namespace chronicles::launcher
