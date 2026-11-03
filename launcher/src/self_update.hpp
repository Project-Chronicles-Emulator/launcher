#pragma once

#include "distribution/distribution.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace chronicles::launcher {

struct ReleaseServer;

// A launcher release may replace only shipped application files, never player data.
void validate_launcher_release(const distribution::Release &release);

// Versions use three decimal components, for example 0.2.5.
int compare_launcher_versions(std::string_view left, std::string_view right);

// Returns true only after a verified replacement worker was started.
bool start_launcher_update(const std::filesystem::path &launcher_directory,
                           const std::string &current_version);

// These explicit-server entry points support a disposable signed test service. Production
// entry points always select the pinned player server themselves.
bool start_launcher_update_for_server(const std::filesystem::path &launcher_directory,
                                      const std::string &current_version,
                                      const ReleaseServer &server);

int run_launcher_update_worker(const std::filesystem::path &launcher_directory,
                               std::uintptr_t inherited_parent_handle,
                               std::uintptr_t inherited_ready_handle);

int run_launcher_update_worker_for_server(const std::filesystem::path &launcher_directory,
                                          std::uintptr_t inherited_parent_handle,
                                          std::uintptr_t inherited_ready_handle,
                                          const ReleaseServer &server);

} // namespace chronicles::launcher
