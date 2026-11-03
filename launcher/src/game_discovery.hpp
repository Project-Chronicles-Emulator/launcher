#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace chronicles::launcher {

// Exposed for fixture tests; reads quoted Valve KeyValues without executing Steam.
std::vector<std::string> valve_values(std::string_view document, std::string_view key);

std::optional<std::filesystem::path> discover_existing_game();

// Searches only the supplied directories and their immediate Chronicles/ArcheAge children.
std::optional<std::filesystem::path> find_managed_game(
    const std::vector<std::filesystem::path> &search_roots, std::string_view channel);

std::optional<std::filesystem::path> find_other_game(
    const std::vector<std::filesystem::path> &search_roots, std::string_view channel);

std::optional<std::filesystem::path> discover_other_game(
    const std::filesystem::path &launcher_directory, std::string_view channel);

} // namespace chronicles::launcher
