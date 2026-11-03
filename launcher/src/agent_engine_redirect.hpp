#pragma once

// Points the client's agent-engine (AI chat) REST client at the emulator.
//
// Static analysis of the researched build (docs/agent-engine.md): the front REST client's
// initializer assigns the literal "http://modena.xlgames.corp:38011" to the FString at offset
// 0x168 of that object, and every agent-engine request concatenates its route onto that FString.
// The literal names an internal XLGAMES host. The launcher finds heap FStrings holding exactly
// this text and rewrites them in place, keeping original-byte backups like the auth redirect.
// The constant itself in the executable image is never changed.

#include "fstring_patch.hpp"
#include "process_memory.hpp"
#include "xorstr.hpp"

#include <cstddef>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace chronicles::launcher {

inline const std::u16string kOfficialAgentEngineAddress =
    xorstr_(u"http://modena.xlgames.corp:38011");

struct AgentEngineRedirectResult {
  std::vector<RedirectTarget> patched;
  std::vector<std::string> failures;
  // FStrings that already hold the replacement (for example after a launcher restart).
  std::size_t already_redirected{};
};

class AgentEngineRedirector {
public:
  // `replacement_address` must be an ASCII http:// URL no longer than the official address, so
  // it always fits the FString's existing buffer. Throws std::invalid_argument otherwise.
  AgentEngineRedirector(ProcessMemory &memory, std::string replacement_address);

  // Scans writable memory for FStrings holding the official address and rewrites each one.
  AgentEngineRedirectResult apply(std::stop_token stop);

  // Restores every target that still holds the replacement.
  RestoreResult restore(std::span<const RedirectTarget> targets);

  const std::string &replacement_address() const;

private:
  // FString headers whose buffer starts at `buffer_address` and holds exactly `text`.
  std::vector<std::uint64_t> headers_holding(std::uint64_t buffer_address, std::u16string_view text,
                                             std::stop_token stop);

  ProcessMemory &memory_;
  std::string replacement_address_;
  std::u16string replacement_text_;
};

// "http://<host>:<port>" for the agent engine of the server at `service_url`.
std::string agent_engine_address(std::string_view service_url, int port);

} // namespace chronicles::launcher
