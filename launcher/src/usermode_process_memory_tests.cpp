#include "usermode_process_memory.hpp"

#include <windows.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <memory>

namespace chronicles::launcher {
namespace {

std::filesystem::path current_executable() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length == path.size()) {
    throw std::runtime_error("Could not identify the test executable");
  }

  path.resize(length);
  return path;
}

TEST(UsermodeProcessMemoryTest, RejectsAnUnexpectedExecutable) {
  EXPECT_THROW(UsermodeProcessMemory(GetCurrentProcessId(), L"C:\\other-game.exe"),
               MemoryAccessError);
}

TEST(UsermodeProcessMemoryTest, ReadsWritesAndScansItsSelectedProcess) {
  constexpr std::size_t kPageSize = 4096;
  constexpr std::uint64_t kPointerValue = 0xDEADBAAD11223344ull;
  constexpr std::u16string_view kMarker = u"ChroniclesTestMemory_7F32A5C9";

  void *allocated = VirtualAlloc(nullptr, kPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  ASSERT_NE(allocated, nullptr);
  const auto release = [](void *memory) {
    VirtualFree(memory, 0, MEM_RELEASE);
  };
  std::unique_ptr<void, decltype(release)> page(allocated, release);

  auto *bytes = static_cast<std::uint8_t *>(allocated);
  std::memcpy(bytes + 128, kMarker.data(), kMarker.size() * sizeof(char16_t));
  std::memcpy(bytes + 256, &kPointerValue, sizeof(kPointerValue));

  UsermodeProcessMemory memory(GetCurrentProcessId(), current_executable());
  const auto base_address = reinterpret_cast<std::uint64_t>(allocated);
  const auto read_back = memory.read(base_address + 128, kMarker.size() * sizeof(char16_t));
  EXPECT_EQ(std::memcmp(read_back.data(), kMarker.data(), read_back.size()), 0);

  const std::array<std::uint8_t, 3> replacement{0x12, 0x34, 0x56};
  memory.write(base_address + 384, replacement);
  EXPECT_EQ(memory.read(base_address + 384, replacement.size()),
            std::vector<std::uint8_t>(replacement.begin(), replacement.end()));

  const auto text_matches = memory.find_utf16(kMarker, {});
  EXPECT_NE(std::find(text_matches.begin(), text_matches.end(), base_address + 128),
            text_matches.end());

  const auto pointer_matches = memory.find_pointer(kPointerValue, {});
  EXPECT_NE(std::find(pointer_matches.begin(), pointer_matches.end(), base_address + 256),
            pointer_matches.end());
}

} // namespace
} // namespace chronicles::launcher
