#include "fstring_patch.hpp"

#include "encoding.hpp"

namespace chronicles::launcher {

RedirectTarget patch_fstring(ProcessMemory &memory, std::string role, std::uint64_t header_address,
                             const ArrayHeader &header, std::u16string_view original,
                             std::string_view replacement, std::int32_t maximum_count) {
  const auto replacement_text = ascii_to_utf16(replacement);
  const auto replacement_bytes = utf16_to_bytes(replacement_text, true);
  const auto replacement_count = static_cast<std::int32_t>(replacement_text.size() + 1);
  if (header.capacity < replacement_count) {
    throw MemoryAccessError("The existing buffer is too small for the replacement address");
  }

  RedirectTarget target;
  target.role = std::move(role);
  target.header_address = header_address;
  target.buffer_address = header.data_address;
  target.original_header_bytes = memory.read(header_address, kArrayHeaderSize);
  target.original_text_bytes = memory.read(header.data_address, header.count * sizeof(char16_t));
  target.original_address = utf16_to_display_ascii(original);
  target.replacement_address = std::string(replacement);

  const auto current_header = parse_array_header(target.original_header_bytes);
  if (current_header.data_address != header.data_address || current_header.count != header.count) {
    throw MemoryAccessError("The string header changed before it could be patched");
  }

  // Text first, then the new length. Capacity and the data pointer stay untouched.
  memory.write(target.buffer_address, replacement_bytes);
  memory.write(header_address + kArrayCountOffset, u32_to_bytes_le(replacement_count));

  const auto verified_header = read_array_header(memory, header_address);
  const auto verified = read_fstring_text(memory, verified_header, maximum_count);
  if (!verified || *verified != replacement_text) {
    throw MemoryAccessError("Readback did not confirm the replacement address");
  }
  return target;
}

std::optional<std::string> restore_fstring(ProcessMemory &memory, const RedirectTarget &target,
                                           std::string_view replacement) {
  if (target.original_header_bytes.size() != kArrayHeaderSize ||
      target.original_text_bytes.size() < sizeof(char16_t)) {
    return "backup record is incomplete";
  }

  const auto replacement_text = ascii_to_utf16(replacement);
  const auto live_header = memory.read(target.header_address, kArrayHeaderSize);
  if (read_u64_le(live_header, 0) != target.buffer_address) {
    return "buffer changed since the patch was applied";
  }

  const auto expected_count = static_cast<std::uint32_t>(replacement_text.size() + 1);
  if (read_u32_le(live_header, kArrayCountOffset) != expected_count) {
    return "length changed since the patch was applied";
  }

  // Compare only the replacement span. The tail of the allocation still holds bytes from the
  // original, longer URL and is intentionally not part of the redirect.
  const auto expected = utf16_to_bytes(replacement_text, true);
  if (memory.read(target.buffer_address, expected.size()) != expected) {
    return "text is no longer the launcher redirect";
  }

  memory.write(target.buffer_address, target.original_text_bytes);
  memory.write(target.header_address, target.original_header_bytes);
  return std::nullopt;
}

} // namespace chronicles::launcher
