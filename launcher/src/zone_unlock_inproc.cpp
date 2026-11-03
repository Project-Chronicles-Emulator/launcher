#include "zone_unlock_inproc.hpp"

#include "zone_unlock.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace chronicles::launcher {
namespace {

// The client rebuilds parts of its zone state while it loads and enters the world, so the
// thread keeps checking instead of applying the gates once.
constexpr DWORD kPollIntervalMilliseconds = 1000;

// The catalog holds a few hundred 264-byte rows; anything far above that is not this layout.
constexpr std::size_t kMaximumReadBytes = 4 * 1024 * 1024;

constexpr wchar_t kDisableFileName[] = L"steam_settings\\disable_zone_unlock.txt";

std::atomic_flag thread_started = ATOMIC_FLAG_INIT;

// Reads or writes game memory that may be unmapped while the client is still starting. The
// structured exception handler turns an access violation into a normal failure. This function
// must not own objects with destructors, which a __try block cannot unwind.
bool copy_guarded(void *destination, const void *source, std::size_t size) {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Memory access for the process this library is loaded into.
class InProcessMemory : public ProcessMemory {
public:
    std::vector<std::uint8_t> read(std::uint64_t address, std::size_t size) override {
        if (size == 0 || size > kMaximumReadBytes) {
            throw MemoryAccessError("Unexpected in-process read size");
        }

        std::vector<std::uint8_t> bytes(size);
        if (!copy_guarded(bytes.data(), reinterpret_cast<const void *>(address), size)) {
            throw MemoryAccessError("Game memory is not readable yet");
        }

        return bytes;
    }

    void write(std::uint64_t address, std::span<const std::uint8_t> bytes) override {
        if (bytes.empty() || bytes.size() > kMaximumReadBytes) {
            throw MemoryAccessError("Unexpected in-process write size");
        }

        if (!copy_guarded(reinterpret_cast<void *>(address), bytes.data(), bytes.size())) {
            throw MemoryAccessError("Game memory is not writable");
        }
    }

    std::vector<std::uint64_t> find_utf16(std::u16string_view, std::stop_token) override {
        throw MemoryAccessError("Memory scans are not available in process");
    }

    std::vector<std::uint64_t> find_pointer(std::uint64_t, std::stop_token) override {
        throw MemoryAccessError("Memory scans are not available in process");
    }
};

std::filesystem::path library_directory() {
    HMODULE module = nullptr;
    const DWORD flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (!GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&start_in_process_zone_unlock),
                            &module)) {
        return {};
    }

    std::wstring path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return {};
    }

    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

bool unlock_is_disabled() {
    const auto directory = library_directory();
    if (directory.empty()) {
        return false;
    }

    std::error_code error;
    return std::filesystem::exists(directory / kDisableFileName, error);
}

// Failures are normal while the client loads, so only a changed message is reported, and only
// to the debugger output.
void report_changed_failure(std::string &last_message, const std::string &message) {
    if (message == last_message) {
        return;
    }

    last_message = message;
    const auto line = "Project Chronicles zone unlock: " + message + "\n";
    OutputDebugStringA(line.c_str());
}

void unlock_once(ZoneUnlocker &unlocker) {
    std::string catalog_failure;
    try {
        const auto inactive_targets = unlocker.plan_all_inactive();
        if (!inactive_targets.empty()) {
            unlocker.apply(inactive_targets);
        }
    } catch (const std::exception &error) {
        // The catalog can be rebuilt mid-session, which makes one pass fail its layout
        // checks. The movement clears below do not depend on the catalog and must still run.
        catalog_failure = error.what();
    }

    unlocker.clear_inactive_anchor();
    unlocker.clear_stale_pending_transition();

    if (!catalog_failure.empty()) {
        throw std::runtime_error(catalog_failure);
    }
}

DWORD WINAPI zone_unlock_thread(LPVOID) {
    InProcessMemory memory;
    ZoneUnlocker unlocker(memory);
    std::string last_failure;

    while (true) {
        try {
            unlock_once(unlocker);
            report_changed_failure(last_failure, "catalog is unlocked");
        } catch (const std::exception &error) {
            report_changed_failure(last_failure, error.what());
        }

        Sleep(kPollIntervalMilliseconds);
    }
}

} // namespace

void start_in_process_zone_unlock() {
    if (thread_started.test_and_set()) {
        return;
    }

    if (unlock_is_disabled()) {
        return;
    }

    HANDLE thread = CreateThread(nullptr, 0, zone_unlock_thread, nullptr, 0, nullptr);
    if (thread != nullptr) {
        CloseHandle(thread);
    }
}

} // namespace chronicles::launcher
