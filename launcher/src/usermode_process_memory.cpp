#include "usermode_process_memory.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <string>

namespace chronicles::launcher {
namespace {

constexpr std::size_t kMaximumRequestBytes = 1024 * 1024;
constexpr std::uint64_t kMaximumScanBytes = 64ull * 1024 * 1024 * 1024;
constexpr std::size_t kMaximumMatches = 1000;
constexpr auto kScanTimeout = std::chrono::minutes(15);

void fail(const char *operation) {
  throw MemoryAccessError(std::string(operation) + " failed (Windows error " +
                          std::to_string(GetLastError()) + ")");
}

bool is_writable_private_region(const MEMORY_BASIC_INFORMATION &region) {
  if (region.State != MEM_COMMIT || region.Type != MEM_PRIVATE ||
      (region.Protect & PAGE_GUARD) != 0) {
    return false;
  }

  const DWORD protection = region.Protect & 0xff;
  return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
         protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

} // namespace

struct UsermodeProcessMemory::Impl {
  HANDLE process{};
  std::uint64_t created{};

  ~Impl() {
    if (process != nullptr) {
      CloseHandle(process);
    }
  }

  void verify_alive() const {
    if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT) {
      throw MemoryAccessError("The selected client exited");
    }
  }
};

UsermodeProcessMemory::UsermodeProcessMemory(
    std::uint32_t pid, const std::filesystem::path &expected_executable)
    : impl_(std::make_unique<Impl>()) {
  impl_->process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ |
                                    PROCESS_VM_WRITE | PROCESS_VM_OPERATION | SYNCHRONIZE,
                                FALSE, pid);
  if (impl_->process == nullptr) {
    fail("OpenProcess");
  }

  std::wstring image_path(32768, L'\0');
  DWORD length = static_cast<DWORD>(image_path.size());
  if (!QueryFullProcessImageNameW(impl_->process, 0, image_path.data(), &length)) {
    fail("QueryFullProcessImageNameW");
  }
  image_path.resize(length);
  if (_wcsicmp(std::filesystem::path(image_path).lexically_normal().c_str(),
               expected_executable.lexically_normal().c_str()) != 0) {
    throw MemoryAccessError("Selected process does not match the configured local game");
  }

  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  if (!GetProcessTimes(impl_->process, &created, &exited, &kernel, &user)) {
    fail("GetProcessTimes");
  }
  impl_->verify_alive();
  impl_->created = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) |
                   created.dwLowDateTime;
}

UsermodeProcessMemory::~UsermodeProcessMemory() = default;

std::uint32_t UsermodeProcessMemory::protect(std::uint64_t address, std::size_t size,
                                             std::uint32_t new_protection) {
  impl_->verify_alive();
  if (address == 0 || size == 0 || size > kMaximumRequestBytes) {
    throw MemoryAccessError("Invalid user-mode protection request");
  }

  DWORD old_protection = 0;
  if (!VirtualProtectEx(impl_->process, reinterpret_cast<void *>(address), size,
                        new_protection, &old_protection)) {
    fail("VirtualProtectEx");
  }

  return old_protection;
}

std::vector<std::uint8_t> UsermodeProcessMemory::read(std::uint64_t address, std::size_t size) {
  impl_->verify_alive();
  if (size > kMaximumRequestBytes) {
    throw MemoryAccessError("User-mode read exceeds the bounded request size");
  }
  if (size == 0) {
    return {};
  }

  std::vector<std::uint8_t> bytes(size);
  SIZE_T transferred = 0;
  if (!ReadProcessMemory(impl_->process, reinterpret_cast<const void *>(address), bytes.data(),
                         size, &transferred) ||
      transferred != size) {
    fail("ReadProcessMemory");
  }
  return bytes;
}

void UsermodeProcessMemory::write(std::uint64_t address,
                                  std::span<const std::uint8_t> bytes) {
  impl_->verify_alive();
  if (bytes.size() > kMaximumRequestBytes) {
    throw MemoryAccessError("User-mode write exceeds the bounded request size");
  }
  if (bytes.empty()) {
    return;
  }

  SIZE_T transferred = 0;
  if (!WriteProcessMemory(impl_->process, reinterpret_cast<void *>(address), bytes.data(),
                          bytes.size(), &transferred) ||
      transferred != bytes.size()) {
    fail("WriteProcessMemory");
  }
}

std::vector<std::uint64_t> UsermodeProcessMemory::find_utf16(std::u16string_view text,
                                                               std::stop_token stop) {
  if (text.empty()) {
    return {};
  }
  if (text.size() > kMaximumRequestBytes / sizeof(char16_t)) {
    throw MemoryAccessError("User-mode text scan exceeds the bounded request size");
  }

  std::vector<std::uint8_t> needle;
  needle.reserve(text.size() * 2);
  for (const char16_t character : text) {
    needle.push_back(static_cast<std::uint8_t>(character));
    needle.push_back(static_cast<std::uint8_t>(character >> 8));
  }
  return scan(needle, 1, stop);
}

std::vector<std::uint64_t> UsermodeProcessMemory::find_pointer(std::uint64_t value,
                                                                 std::stop_token stop) {
  std::uint8_t needle[sizeof(value)]{};
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    needle[index] = static_cast<std::uint8_t>(value >> (index * 8));
  }
  return scan(needle, alignof(std::uint64_t), stop);
}

std::vector<std::uint64_t> UsermodeProcessMemory::scan(std::span<const std::uint8_t> needle,
                                                        std::size_t alignment,
                                                        std::stop_token stop) {
  impl_->verify_alive();
  if (needle.empty() || needle.size() > kMaximumRequestBytes || alignment == 0) {
    throw MemoryAccessError("Invalid user-mode scan request");
  }

  SYSTEM_INFO system_info{};
  GetNativeSystemInfo(&system_info);
  const auto maximum_address =
      reinterpret_cast<std::uintptr_t>(system_info.lpMaximumApplicationAddress);
  const auto deadline = std::chrono::steady_clock::now() + kScanTimeout;
  std::uint64_t scanned_bytes = 0;
  std::vector<std::uint64_t> matches;

  for (std::uintptr_t address = 0; address < maximum_address;) {
    if (stop.stop_requested()) {
      throw OperationCancelled();
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      throw MemoryAccessError("User-mode memory scan exceeded 15 minutes");
    }
    impl_->verify_alive();

    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQueryEx(impl_->process, reinterpret_cast<const void *>(address), &region,
                       sizeof(region)) == 0) {
      if (GetLastError() == ERROR_INVALID_PARAMETER) {
        break;
      }
      fail("VirtualQueryEx");
    }

    const auto region_start = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (region_start > maximum_address || region.RegionSize > maximum_address - region_start) {
      break;
    }
    const auto region_end = region_start + region.RegionSize;
    if (region_end <= address) {
      throw MemoryAccessError("VirtualQueryEx did not advance the scan");
    }
    address = region_end;
    if (!is_writable_private_region(region)) {
      continue;
    }
    if (region.RegionSize > kMaximumScanBytes - scanned_bytes) {
      throw MemoryAccessError("User-mode memory scan exceeded 64 GiB");
    }
    scanned_bytes += region.RegionSize;

    std::vector<std::uint8_t> carry;
    for (std::uintptr_t chunk_start = region_start; chunk_start < region_end;) {
      if (stop.stop_requested()) {
        throw OperationCancelled();
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        throw MemoryAccessError("User-mode memory scan exceeded 15 minutes");
      }
      impl_->verify_alive();

      const auto chunk_size =
          (std::min)(kMaximumRequestBytes, static_cast<std::size_t>(region_end - chunk_start));
      std::vector<std::uint8_t> bytes(carry.size() + chunk_size);
      std::copy(carry.begin(), carry.end(), bytes.begin());
      SIZE_T transferred = 0;
      const BOOL read_ok = ReadProcessMemory(impl_->process,
                                             reinterpret_cast<const void *>(chunk_start),
                                             bytes.data() + carry.size(), chunk_size, &transferred);
      if (!read_ok) {
        fail("ReadProcessMemory during scan");
      }
      if (transferred != chunk_size) {
        throw MemoryAccessError("Partial user-mode scan read at address " +
                                std::to_string(chunk_start));
      }

      const auto base_address = chunk_start - carry.size();
      auto candidate = bytes.begin();
      while ((candidate = std::search(candidate, bytes.end(), needle.begin(), needle.end())) !=
             bytes.end()) {
        const auto found_address =
            base_address + static_cast<std::size_t>(candidate - bytes.begin());
        if (found_address % alignment == 0) {
          matches.push_back(found_address);
          if (matches.size() > kMaximumMatches) {
            throw MemoryAccessError("User-mode memory scan found too many matches");
          }
        }
        ++candidate;
      }

      const auto carry_size = (std::min)(needle.size() - 1, bytes.size());
      carry.assign(bytes.end() - carry_size, bytes.end());
      chunk_start += chunk_size;
    }
  }
  return matches;
}

std::uint64_t UsermodeProcessMemory::creation_time() const {
  return impl_->created;
}

} // namespace chronicles::launcher
