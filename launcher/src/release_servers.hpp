#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace chronicles::launcher {

struct ReleaseServer {
  std::string id;
  std::string name;
  std::string origin;
  std::string patch_public_key;
  std::string channel;
  bool local{};
  std::string world_host;
  int world_port{8081};
};

// Profiles default to loopback. Hosted endpoints and signing keys are operator configuration.
std::vector<ReleaseServer> release_servers(const std::filesystem::path &directory);

} // namespace chronicles::launcher
