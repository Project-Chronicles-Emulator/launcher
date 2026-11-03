#pragma once

#include "distribution/distribution.hpp"

#include <filesystem>
#include <span>
#include <vector>

namespace chronicles::launcher {

std::vector<distribution::PatchFile>
legacy_game_runtime_files(const std::filesystem::path &launcher_directory);

// Only the exact, signed 0.2.0 runtime-file mistakes are excluded. All retained
// game files still use their original signed hashes and rollback identity.
void exclude_legacy_game_runtimes(distribution::Release &release,
                                  std::span<const distribution::PatchFile> legacy_files);

// Refuse a managed launch when its signed Steam initialization payload is absent or altered.
void require_managed_steam_compatibility(const distribution::Release &release,
                                         const std::filesystem::path &game_directory);

// Moves exact receipt matches out of a managed game. Unknown bytes are preserved
// and rejected; originals remain available in a sibling backup directory.
std::filesystem::path
quarantine_managed_game_files(const std::filesystem::path &game_directory,
                              std::span<const distribution::PatchFile> owned_files);

} // namespace chronicles::launcher
