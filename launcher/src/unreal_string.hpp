#pragma once

#include "process_memory.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace chronicles::launcher {

// Unreal Engine TArray layout on x64: data pointer, element count, allocated capacity.
// FString is TArray<TCHAR>; its count includes the terminating NUL.
struct ArrayHeader {
  std::uint64_t data_address{};
  std::int32_t count{};
  std::int32_t capacity{};
};

inline constexpr std::size_t kArrayHeaderSize = 16;

inline constexpr std::uint64_t kArrayCountOffset = 8;

ArrayHeader parse_array_header(std::span<const std::uint8_t> bytes);

ArrayHeader read_array_header(ProcessMemory &memory, std::uint64_t header_address);

// Returns the string without its terminator. Returns nullopt for an empty FString, an implausible
// header, or a buffer that is not NUL-terminated exactly at count - 1.
std::optional<std::u16string> read_fstring_text(ProcessMemory &memory, const ArrayHeader &header,
                                                std::int32_t max_count);

// Reads a NUL-terminated UTF-16 string without crossing into an unreadable following page when
// the string ends earlier. Returns nullopt if no terminator appears within max_characters.
std::optional<std::u16string> read_utf16_until_nul(ProcessMemory &memory, std::uint64_t address,
                                                   std::size_t max_characters);

} // namespace chronicles::launcher
