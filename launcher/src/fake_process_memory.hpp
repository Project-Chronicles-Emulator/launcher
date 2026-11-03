#pragma once

// In-memory ProcessMemory for launcher tests: sparse committed pages, byte-exact scans.

#include "encoding.hpp"
#include "process_memory.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace chronicles::launcher::test_support {

class FakeMemory : public ProcessMemory {
public:
  std::vector<std::uint8_t> read(std::uint64_t address, std::size_t size) override {
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t index = 0; index < size; ++index) {
      bytes[index] = byte_at(address + index);
    }
    return bytes;
  }

  void write(std::uint64_t address, std::span<const std::uint8_t> bytes) override {
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      const auto target = address + index;
      const auto existing = bytes_.find(target);
      if (existing == bytes_.end()) {
        throw MemoryAccessError("Write outside allocated memory at " + format_address(target));
      }
      existing->second = bytes[index];
    }
    ++write_count;
  }

  std::vector<std::uint64_t> find_utf16(std::u16string_view text, std::stop_token) override {
    const auto needle = utf16_to_bytes(text, false);
    return find_bytes(needle);
  }

  std::vector<std::uint64_t> find_pointer(std::uint64_t value, std::stop_token) override {
    std::vector<std::uint8_t> needle(sizeof(std::uint64_t));
    for (std::size_t index = 0; index < needle.size(); ++index) {
      needle[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xFF);
    }

    auto matches = find_bytes(needle);
    std::erase_if(matches, [](std::uint64_t address) { return address % 8 != 0; });
    return matches;
  }

  std::uint64_t allocate(std::uint64_t address, std::size_t size) {
    // Model committed pages, not just the requested bytes: the real client heap is readable to
    // the end of the page containing a string.
    const auto first_page = address & ~std::uint64_t{0xFFF};
    const auto last_page = (address + size + 0xFFF) & ~std::uint64_t{0xFFF};
    for (auto page = first_page; page < last_page; page += 0x1000) {
      for (std::size_t index = 0; index < 0x1000; ++index) {
        bytes_.try_emplace(page + index, 0);
      }
    }
    return address;
  }

  std::uint64_t allocate_string(std::uint64_t address, std::u16string_view text,
                                std::int32_t capacity) {
    allocate(address, static_cast<std::size_t>(capacity) * sizeof(char16_t));
    const auto bytes = utf16_to_bytes(text, true);
    write(address, bytes);
    return address;
  }

  void write_u64(std::uint64_t address, std::uint64_t value) {
    std::vector<std::uint8_t> bytes(sizeof(std::uint64_t));
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      bytes[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xFF);
    }
    write(address, bytes);
  }

  void write_u32(std::uint64_t address, std::uint32_t value) {
    write(address, u32_to_bytes_le(value));
  }

  int write_count{};

private:
  std::uint8_t byte_at(std::uint64_t address) const {
    const auto existing = bytes_.find(address);
    if (existing == bytes_.end()) {
      throw MemoryAccessError("Read outside allocated memory at " + format_address(address));
    }
    return existing->second;
  }

  std::vector<std::uint64_t> find_bytes(const std::vector<std::uint8_t> &needle) const {
    std::vector<std::uint64_t> matches;
    for (const auto &[address, value] : bytes_) {
      if (value != needle.front()) {
        continue;
      }

      bool found = true;
      for (std::size_t index = 1; index < needle.size(); ++index) {
        const auto next = bytes_.find(address + index);
        if (next == bytes_.end() || next->second != needle[index]) {
          found = false;
          break;
        }
      }
      if (found) {
        matches.push_back(address);
      }
    }
    return matches;
  }

  std::map<std::uint64_t, std::uint8_t> bytes_;
};

} // namespace chronicles::launcher::test_support
