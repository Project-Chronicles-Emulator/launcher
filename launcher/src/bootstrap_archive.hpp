#pragma once

#include "distribution/distribution.hpp"
#include <filesystem>
#include <stop_token>
#include <string_view>

namespace chronicles::launcher {

// Builds the verified local IniBootstrap archive from the extracted demo configuration. The
// generator keeps the original packaged cache opaque and checks its pinned source hash. A
// launch-grant URL must use redacted evidence because its path becomes a reconnect secret.
std::filesystem::path prepare_bootstrap_archive(const std::filesystem::path &project_root,
                                                const std::filesystem::path &run_directory,
                                                const std::filesystem::path &certificate,
                                                std::string_view bootstrap_url,
                                                bool redact_bootstrap_url = false);

std::filesystem::path prepare_signed_bootstrap(const distribution::Release &release,
                                               const std::filesystem::path &run_directory,
                                               const std::string &bootstrap_url,
                                               std::stop_token stop);

} // namespace chronicles::launcher
