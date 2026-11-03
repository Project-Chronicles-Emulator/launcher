#pragma once

#include "process_memory.hpp"

#include <filesystem>
#include <memory>

namespace chronicles::launcher {

// Ordinary Windows process access. No bridge, driver, privilege adjustment or fallback backend.
class UsermodeProcessMemory : public ProcessMemory {
public:
  UsermodeProcessMemory(std::uint32_t pid, const std::filesystem::path &expected_executable);
  ~UsermodeProcessMemory() override;

  std::vector<std::uint8_t> read(std::uint64_t address, std::size_t size) override;
  void write(std::uint64_t address, std::span<const std::uint8_t> bytes) override;
  std::vector<std::uint64_t> find_utf16(std::u16string_view text, std::stop_token stop) override;
  std::vector<std::uint64_t> find_pointer(std::uint64_t value, std::stop_token stop) override;

  // Temporarily change a verified client's page protection for a guarded code write.
  std::uint32_t protect(std::uint64_t address, std::size_t size,
                        std::uint32_t new_protection);

  std::uint64_t creation_time() const;

private:
  std::vector<std::uint64_t> scan(std::span<const std::uint8_t> needle, std::size_t alignment,
                                  std::stop_token stop);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace chronicles::launcher
