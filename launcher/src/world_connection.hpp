#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace chronicles::launcher {

// Resolves the selected world host once, then checks game-owned TCP connections against its
// addresses and port. A helper never accepts an unrelated connection on the same port.
class WorldConnectionTarget {
public:
  WorldConnectionTarget(const std::string &host, int port);

  bool has_connection(std::uint32_t client_pid) const;

  const std::string &host() const;

  int port() const;

private:
  std::string host_;
  int port_{};
  std::vector<std::array<std::uint8_t, 4>> ipv4_addresses_;
  std::vector<std::array<std::uint8_t, 16>> ipv6_addresses_;
};

// Checks the owning PID as well as the loopback peer, so unrelated local TLS
// connections cannot trigger a world-entry memory change.
bool has_local_world_connection(std::uint32_t client_pid, int world_port);

} // namespace chronicles::launcher
