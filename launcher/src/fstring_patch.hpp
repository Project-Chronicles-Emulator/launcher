#pragma once

// In-place rewriting of client FString endpoints, shared by the authentication redirect and the
// agent-engine redirect. A patch keeps the FString's buffer and capacity, writes the replacement
// text and then the new length, and verifies the result by reading it back. Every patch records
// the original header and text bytes so it can be undone.

#include "process_memory.hpp"
#include "unreal_string.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chronicles::launcher {

// One FString rewritten in the client, with everything needed to put it back.
struct RedirectTarget {
  std::string role;
  std::uint64_t header_address{};
  std::uint64_t buffer_address{};
  std::vector<std::uint8_t> original_header_bytes;
  std::vector<std::uint8_t> original_text_bytes;
  std::string original_address;
  // The text written by the patch. Empty in backups made before this field existed; restore then
  // uses the caller's replacement.
  std::string replacement_address;
};

struct RestoreResult {
  int restored{};
  std::vector<std::string> skipped;
};

// Rewrites the FString at `header_address`, whose current header is `header` and whose text is
// `original`. Throws MemoryAccessError when the buffer cannot hold the replacement, the header
// changed before the write, or the readback does not show the replacement.
RedirectTarget patch_fstring(ProcessMemory &memory, std::string role, std::uint64_t header_address,
                             const ArrayHeader &header, std::u16string_view original,
                             std::string_view replacement, std::int32_t maximum_count);

// Puts the original bytes back when the FString still holds the launcher's replacement text and
// the same buffer. Returns the reason when the string was left unchanged.
std::optional<std::string> restore_fstring(ProcessMemory &memory, const RedirectTarget &target,
                                           std::string_view replacement);

} // namespace chronicles::launcher
