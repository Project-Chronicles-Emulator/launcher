#include "unreal_string.hpp"

#include "encoding.hpp"

#include <algorithm>

namespace chronicles::launcher {
namespace {

constexpr std::uint64_t kPageSize = 0x1000;

} // namespace

ArrayHeader parse_array_header(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < kArrayHeaderSize) {
    throw MemoryAccessError("Array header is truncated");
  }

  ArrayHeader header;
  header.data_address = read_u64_le(bytes, 0);
  header.count = static_cast<std::int32_t>(read_u32_le(bytes, kArrayCountOffset));
  header.capacity = static_cast<std::int32_t>(read_u32_le(bytes, kArrayCountOffset + 4));
  return header;
}

ArrayHeader read_array_header(ProcessMemory &memory, std::uint64_t header_address) {
  const auto bytes = memory.read(header_address, kArrayHeaderSize);
  return parse_array_header(bytes);
}

std::optional<std::u16string> read_fstring_text(ProcessMemory &memory, const ArrayHeader &header,
                                                std::int32_t max_count) {
  const bool plausible = header.data_address != 0 && header.count >= 1 &&
                         header.count <= max_count && header.capacity >= header.count;
  if (!plausible) {
    return std::nullopt;
  }

  const auto byte_count = static_cast<std::size_t>(header.count) * sizeof(char16_t);
  const auto bytes = memory.read(header.data_address, byte_count);
  auto text = utf16_from_bytes(bytes);
  if (text.back() != u'\0') {
    return std::nullopt;
  }

  text.pop_back();
  if (text.find(u'\0') != std::u16string::npos) {
    return std::nullopt;
  }
  return text;
}

std::optional<std::u16string> read_utf16_until_nul(ProcessMemory &memory, std::uint64_t address,
                                                   std::size_t max_characters) {
  std::u16string text;
  std::uint64_t cursor = address;
  while (text.size() < max_characters) {
    // Read to the end of the current page first; later pages are only touched when needed.
    const auto bytes_to_page_end = kPageSize - (cursor % kPageSize);
    const auto bytes_remaining = (max_characters - text.size()) * sizeof(char16_t);
    auto chunk_size = std::min<std::uint64_t>(bytes_to_page_end, bytes_remaining);
    chunk_size -= chunk_size % sizeof(char16_t);
    if (chunk_size == 0) {
      chunk_size = sizeof(char16_t);
    }

    const auto chunk = utf16_from_bytes(memory.read(cursor, static_cast<std::size_t>(chunk_size)));
    const auto terminator = chunk.find(u'\0');
    if (terminator != std::u16string::npos) {
      text.append(chunk, 0, terminator);
      return text;
    }

    text.append(chunk);
    cursor += chunk_size;
  }

  return std::nullopt;
}

} // namespace chronicles::launcher
