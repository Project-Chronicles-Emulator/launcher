#include "agent_engine_redirect.hpp"

#include "distribution/distribution.hpp"
#include "encoding.hpp"
#include "unreal_string.hpp"

#include <stdexcept>

namespace chronicles::launcher {
namespace {

constexpr std::string_view kHttpScheme = "http://";

// Large enough for any address this module reads or writes.
constexpr std::int32_t kMaximumAddressCharacters = 128;

} // namespace

AgentEngineRedirector::AgentEngineRedirector(ProcessMemory &memory, std::string replacement_address)
    : memory_(memory), replacement_address_(std::move(replacement_address)),
      replacement_text_(ascii_to_utf16(replacement_address_)) {
  if (!replacement_address_.starts_with(kHttpScheme)) {
    throw std::invalid_argument("The agent-engine address must be an http:// URL");
  }
  // The client's buffer was sized for the official literal; a longer URL could not fit.
  if (replacement_text_.size() > kOfficialAgentEngineAddress.size()) {
    throw std::invalid_argument("The agent-engine address is longer than the client's buffer");
  }
}

std::vector<std::uint64_t> AgentEngineRedirector::headers_holding(std::uint64_t buffer_address,
                                                                  std::u16string_view text,
                                                                  std::stop_token stop) {
  std::vector<std::uint64_t> headers;
  for (const auto header_address : memory_.find_pointer(buffer_address, stop)) {
    try {
      const auto header = read_array_header(memory_, header_address);
      if (header.data_address != buffer_address ||
          header.count != static_cast<std::int32_t>(text.size() + 1)) {
        continue;
      }
      const auto current = read_fstring_text(memory_, header, kMaximumAddressCharacters);
      if (current && *current == text) {
        headers.push_back(header_address);
      }
    } catch (const MemoryAccessError &) {
      // A stale pointer-sized value next to unreadable memory is not an FString header.
    }
  }
  return headers;
}

AgentEngineRedirectResult AgentEngineRedirector::apply(std::stop_token stop) {
  AgentEngineRedirectResult result;

  for (const auto buffer : memory_.find_utf16(kOfficialAgentEngineAddress, stop)) {
    for (const auto header_address : headers_holding(buffer, kOfficialAgentEngineAddress, stop)) {
      if (stop.stop_requested()) {
        throw OperationCancelled();
      }
      try {
        const auto header = read_array_header(memory_, header_address);
        result.patched.push_back(patch_fstring(memory_, "agent engine address", header_address,
                                               header, kOfficialAgentEngineAddress,
                                               replacement_address_, kMaximumAddressCharacters));
      } catch (const std::exception &error) {
        result.failures.push_back(std::string("Agent engine address: ") + error.what());
      }
    }
  }

  if (result.patched.empty()) {
    for (const auto buffer : memory_.find_utf16(replacement_text_, stop)) {
      result.already_redirected += headers_holding(buffer, replacement_text_, stop).size();
    }
  }
  return result;
}

RestoreResult AgentEngineRedirector::restore(std::span<const RedirectTarget> targets) {
  RestoreResult result;
  for (const auto &target : targets) {
    try {
      if (const auto reason = restore_fstring(memory_, target, replacement_address_)) {
        result.skipped.push_back(target.role + ": " + *reason);
        continue;
      }
      ++result.restored;
    } catch (const std::exception &error) {
      result.skipped.push_back(target.role + ": " + error.what());
    }
  }
  return result;
}

const std::string &AgentEngineRedirector::replacement_address() const {
  return replacement_address_;
}

std::string agent_engine_address(std::string_view service_url, int port) {
  const auto origin = distribution::parse_service_origin(service_url);
  return std::string(kHttpScheme) + origin.host + ":" + std::to_string(port);
}

} // namespace chronicles::launcher
