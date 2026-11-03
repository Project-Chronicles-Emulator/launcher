#pragma once

#include "encoding.hpp"
#include "process_memory.hpp"

#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace chronicles::launcher {

// OpenSSL 1.1.1t layout recovered from build 40367e24. Call only after the helper's
// executable and live-code guards pass. SNI alone does not enable name verification.
inline void require_hosted_tls_identity(ProcessMemory &memory, std::uint64_t ssl,
                                        std::string_view expected_host) {
  if (expected_host.empty() || expected_host.size() > 253 ||
      expected_host.find('\0') != std::string_view::npos) {
    throw std::runtime_error("Invalid selected world certificate hostname");
  }

  constexpr std::uint64_t kSslParametersOffset = 0xD0;
  const auto parameter_pointer = memory.read(ssl + kSslParametersOffset, 8);
  const auto parameters = read_u64_le(parameter_pointer, 0);
  if (parameters == 0) {
    throw std::runtime_error("World TLS has no certificate verification parameters");
  }

  const auto fields = memory.read(parameters, 0x68);
  const auto inheritance_flags = read_u32_le(fields, 0x10);
  const auto verification_flags = read_u32_le(fields, 0x14);
  const auto hosts = read_u64_le(fields, 0x30);
  const auto host_flags = read_u32_le(fields, 0x38);

  // Permit ordinary verification and stricter hostname options only. In particular,
  // do not allow overridden time checks, ignored critical extensions or partial trust.
  constexpr std::uint32_t kTrustedFirst = 0x8000;
  constexpr std::uint32_t kStrictHostFlags = 0x02 | 0x04 | 0x20;
  if (inheritance_flags != 0 || (verification_flags & ~kTrustedFirst) != 0 ||
      (host_flags & ~kStrictHostFlags) != 0) {
    throw std::runtime_error("World TLS verification parameters are not supported safely");
  }
  if (hosts == 0) {
    throw std::runtime_error("World TLS is missing the selected server's certificate hostname");
  }

  const auto host_stack = memory.read(hosts, 16);
  const auto host_count = read_u32_le(host_stack, 0);
  const auto host_data = read_u64_le(host_stack, 8);
  if (host_count != 1 || host_data == 0) {
    throw std::runtime_error("World TLS must verify exactly the selected server hostname");
  }

  const auto name_pointer = memory.read(host_data, 8);
  const auto name = read_u64_le(name_pointer, 0);
  if (name == 0) {
    throw std::runtime_error("World TLS certificate hostname is missing");
  }
  const auto hostname = memory.read(name, expected_host.size() + 1);
  if (hostname.size() != expected_host.size() + 1 || hostname.back() != 0) {
    throw std::runtime_error("World TLS certificate hostname differs from the selected server");
  }

  const auto lowercase_ascii = [](unsigned char value) {
    if (value >= 'A' && value <= 'Z') {
      return static_cast<unsigned char>(value + ('a' - 'A'));
    }
    return value;
  };
  for (std::size_t index = 0; index < expected_host.size(); ++index) {
    if (lowercase_ascii(hostname[index]) !=
        lowercase_ascii(static_cast<unsigned char>(expected_host[index]))) {
      throw std::runtime_error("World TLS certificate hostname differs from the selected server");
    }
  }
}

} // namespace chronicles::launcher
