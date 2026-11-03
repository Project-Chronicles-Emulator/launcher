#include "world_connection.hpp"

// Winsock2 must be included before Windows networking headers to avoid winsock.h conflicts.
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace chronicles::launcher {
namespace {

class WinsockSession {
public:
  WinsockSession() {
    WSADATA winsock_data{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
      throw std::runtime_error("Could not initialize Windows address resolution");
    }
  }

  ~WinsockSession() {
    WSACleanup();
  }
};

bool valid_world_host(const std::string &host) {
  if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.') {
    return false;
  }

  bool previous_dot = false;
  for (const unsigned char character : host) {
    const bool allowed =
        (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '-' || character == '.';
    if (!allowed || (character == '.' && previous_dot)) {
      return false;
    }
    previous_dot = character == '.';
  }
  return true;
}

std::vector<std::uint8_t> read_tcp_table(ULONG address_family) {
  DWORD required_bytes = 0;
  const auto first_result = GetExtendedTcpTable(nullptr, &required_bytes, FALSE, address_family,
                                                TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
  if (first_result == NO_ERROR) {
    return {};
  }
  if (first_result != ERROR_INSUFFICIENT_BUFFER) {
    throw std::runtime_error("Could not size the Windows TCP connection table");
  }

  for (int attempt = 0; attempt < 3; ++attempt) {
    std::vector<std::uint8_t> storage(required_bytes);
    const auto result = GetExtendedTcpTable(storage.data(), &required_bytes, FALSE, address_family,
                                            TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
    if (result == NO_ERROR) {
      return storage;
    }
    if (result != ERROR_INSUFFICIENT_BUFFER) {
      throw std::runtime_error("Could not read the Windows TCP connection table");
    }
  }
  throw std::runtime_error("The Windows TCP connection table kept changing size");
}

template <std::size_t AddressSize>
void add_address(std::vector<std::array<std::uint8_t, AddressSize>> &addresses, const void *bytes) {
  std::array<std::uint8_t, AddressSize> address{};
  std::memcpy(address.data(), bytes, AddressSize);
  if (std::find(addresses.begin(), addresses.end(), address) == addresses.end()) {
    addresses.push_back(address);
  }
}

} // namespace

WorldConnectionTarget::WorldConnectionTarget(const std::string &host, int port)
    : host_(host), port_(port) {
  if (!valid_world_host(host_) || port_ < 1 || port_ > 65535) {
    throw std::invalid_argument("World connection requires a DNS host and valid TCP port");
  }

  WinsockSession winsock_session;

  ADDRINFOW hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  const std::wstring wide_host(host_.begin(), host_.end());
  ADDRINFOW *raw_addresses = nullptr;
  const int result = GetAddrInfoW(wide_host.c_str(), nullptr, &hints, &raw_addresses);
  std::unique_ptr<ADDRINFOW, decltype(&FreeAddrInfoW)> addresses(raw_addresses, FreeAddrInfoW);
  if (result == 0) {
    for (auto *entry = addresses.get(); entry != nullptr; entry = entry->ai_next) {
      if (entry->ai_family == AF_INET && entry->ai_addrlen >= sizeof(sockaddr_in)) {
        const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(entry->ai_addr);
        add_address(ipv4_addresses_, &ipv4->sin_addr);
      } else if (entry->ai_family == AF_INET6 && entry->ai_addrlen >= sizeof(sockaddr_in6)) {
        const auto *ipv6 = reinterpret_cast<const sockaddr_in6 *>(entry->ai_addr);
        add_address(ipv6_addresses_, &ipv6->sin6_addr);
      }
    }
  }
  addresses.reset();

  if (result != 0 || (ipv4_addresses_.empty() && ipv6_addresses_.empty())) {
    throw std::runtime_error("Could not resolve the selected world host: " + host_);
  }
}

bool WorldConnectionTarget::has_connection(std::uint32_t client_pid) const {
  if (!ipv4_addresses_.empty()) {
    const auto storage = read_tcp_table(AF_INET);
    if (!storage.empty()) {
      const auto *table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID *>(storage.data());
      for (DWORD index = 0; index < table->dwNumEntries; ++index) {
        const auto &row = table->table[index];
        if (row.dwOwningPid != client_pid || row.dwState != MIB_TCP_STATE_ESTAB ||
            ntohs(static_cast<u_short>(row.dwRemotePort)) != port_) {
          continue;
        }

        std::array<std::uint8_t, 4> remote_address{};
        std::memcpy(remote_address.data(), &row.dwRemoteAddr, remote_address.size());
        if (std::find(ipv4_addresses_.begin(), ipv4_addresses_.end(), remote_address) !=
            ipv4_addresses_.end()) {
          return true;
        }
      }
    }
  }

  if (!ipv6_addresses_.empty()) {
    const auto storage = read_tcp_table(AF_INET6);
    if (!storage.empty()) {
      const auto *table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID *>(storage.data());
      for (DWORD index = 0; index < table->dwNumEntries; ++index) {
        const auto &row = table->table[index];
        if (row.dwOwningPid != client_pid || row.dwState != MIB_TCP_STATE_ESTAB ||
            ntohs(static_cast<u_short>(row.dwRemotePort)) != port_) {
          continue;
        }

        std::array<std::uint8_t, 16> remote_address{};
        std::memcpy(remote_address.data(), row.ucRemoteAddr, remote_address.size());
        if (std::find(ipv6_addresses_.begin(), ipv6_addresses_.end(), remote_address) !=
            ipv6_addresses_.end()) {
          return true;
        }
      }
    }
  }
  return false;
}

const std::string &WorldConnectionTarget::host() const {
  return host_;
}

int WorldConnectionTarget::port() const {
  return port_;
}

bool has_local_world_connection(std::uint32_t client_pid, int world_port) {
  const WorldConnectionTarget loopback("127.0.0.1", world_port);
  return loopback.has_connection(client_pid);
}

} // namespace chronicles::launcher
