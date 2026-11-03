#include "usermode_process_memory.hpp"
#include "zone_unlock.hpp"

#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>

namespace {

using namespace chronicles::launcher;
using Json = nlohmann::json;

std::uint32_t selected_pid = 0;

// The client builds its zone catalog only after it finishes loading, so a fresh launch may
// need a long wait before the catalog can be read.
constexpr unsigned kMaximumWaitSeconds = 1800;

bool client_has_exited(std::uint32_t pid) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) {
        return true;
    }

    const bool exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    CloseHandle(process);
    return exited;
}

Json encode_targets(std::span<const ZoneUnlockTarget> targets) {
    auto rows = Json::array();
    for (const auto &target : targets) {
        rows.push_back({{"id", target.zone.id}, {"key", target.zone.key},
                        {"row_address", target.row_address}, {"key_value", target.key_value},
                        {"original_activation", target.original_activation},
                        {"original_field_access", target.original_field_access}});
    }
    return rows;
}

std::vector<ZoneUnlockTarget> decode_targets(const Json &rows) {
    if (!rows.is_array() || rows.size() > 4096) {
        throw std::runtime_error("Invalid zone backup target count");
    }
    std::vector<ZoneUnlockTarget> targets;
    for (const auto &row : rows) {
        const auto &original = row.at("original_activation");
        if (!original.is_number_integer() || original.get<std::int64_t>() < 0 ||
            original.get<std::int64_t>() > 1) {
            throw std::runtime_error("Invalid original zone activation byte");
        }
        const auto id = row.at("id").get<std::int64_t>();
        const auto &field_access = row.at("original_field_access");
        if (!field_access.is_number_integer() || field_access.get<std::int64_t>() < 0 ||
            field_access.get<std::int64_t>() > 1) {
            throw std::runtime_error("Invalid original zone map-eligibility byte");
        }
        if (id <= 0 || id > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("Invalid zone backup identifier");
        }
        targets.push_back({{static_cast<std::uint16_t>(id), row.at("key").get<std::string>()},
                            row.at("row_address").get<std::uint64_t>(),
                            row.at("key_value").get<std::uint64_t>(),
                            original.get<std::uint8_t>(), field_access.get<std::uint8_t>()});
    }
    return targets;
}

int run(int argc, wchar_t **argv) {
    std::uint32_t pid = 0;
    std::filesystem::path directory;
    bool restore = false;
    bool inspect = false;
    unsigned wait_seconds = 0;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--pid" && index + 1 < argc) {
            std::size_t parsed = 0;
            const std::wstring value(argv[++index]);
            const auto number = std::stoull(value, &parsed);
            if (parsed != value.size() || number == 0 ||
                number > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument("Invalid client PID");
            }
            pid = static_cast<std::uint32_t>(number);
            selected_pid = pid;
        } else if (argument == L"--client-directory" && index + 1 < argc) {
            directory = std::filesystem::absolute(argv[++index]);
        } else if (argument == L"--wait-seconds" && index + 1 < argc) {
            std::size_t parsed = 0;
            const std::wstring value(argv[++index]);
            const auto number = std::stoul(value, &parsed);
            if (parsed != value.size() || number > kMaximumWaitSeconds) {
                throw std::invalid_argument("Wait must be between 0 and 1800 seconds");
            }
            wait_seconds = static_cast<unsigned>(number);
        } else if (argument == L"--restore") {
            restore = true;
        } else if (argument == L"--inspect") {
            inspect = true;
        } else {
            throw std::invalid_argument("Unknown zone helper argument");
        }
    }
    if (pid == 0 || directory.empty() || (restore && inspect)) {
        throw std::invalid_argument(
            "Usage: chronicles-zone-unlock --pid ID --client-directory PATH "
            "[--inspect|--restore] [--wait-seconds 0..1800]");
    }

    const auto executable = directory / "X5/Binaries/Win64/ArcheAgeChronicles.exe";
    UsermodeProcessMemory memory(pid, executable);
    ZoneUnlocker unlocker(memory);
    const auto backup_directory = std::filesystem::current_path() / "out/launcher";
    const auto backup_path = backup_directory /
        ("all-zones-" + std::to_string(pid) + "-" +
         std::to_string(memory.creation_time()) + ".json");
    std::vector<ZoneUnlockTarget> prior_targets;
    if (std::filesystem::exists(backup_path)) {
        if (std::filesystem::file_size(backup_path) > 1024 * 1024) {
            throw std::runtime_error("Original-zone backup is oversized");
        }
        std::ifstream input(backup_path);
        const auto backup = Json::parse(input);
        if (backup.at("pid") != pid || backup.at("created") != memory.creation_time()) {
            throw std::runtime_error("Zone backup belongs to another client process");
        }
        prior_targets = decode_targets(backup.at("targets"));
    }
    if (restore) {
        if (!std::filesystem::exists(backup_path)) {
            throw std::runtime_error("No original-zone backup exists for this client");
        }
        unlocker.restore(prior_targets);
        std::cout << "Restored " << prior_targets.size() << " zone activation bytes using user mode.\n";
        return 0;
    }

    std::vector<ZoneUnlockTarget> targets;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(wait_seconds);
    while (true) {
        try {
            targets = unlocker.plan_all_inactive();
            break;
        } catch (const MemoryAccessError &) {
            if (std::chrono::steady_clock::now() >= deadline || client_has_exited(pid)) {
                throw;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    if (inspect) {
        std::cout << "User-mode inspection passed: " << targets.size() << " inactive zones.\n";
        return 0;
    }

    const auto newly_inactive = targets.size();
    for (const auto &old : prior_targets) {
        const auto existing = std::ranges::find(targets, old.zone.id,
            [](const auto &target) {
                return target.zone.id;
            });
        if (existing == targets.end()) {
            targets.push_back(old);
        } else {
            if (existing->row_address != old.row_address || existing->key_value != old.key_value ||
                existing->zone.key != old.zone.key) {
                throw std::runtime_error("Zone catalog moved since its original-byte backup");
            }
            existing->original_activation = old.original_activation;
            existing->original_field_access = old.original_field_access;
        }
    }
    std::filesystem::create_directories(backup_directory);
    const Json backup{{"pid", pid}, {"created", memory.creation_time()},
                       {"targets", encode_targets(targets)}};
    const auto staged_backup = backup_path.wstring() + L".tmp";
    {
        std::ofstream output(staged_backup, std::ios::trunc);
        output << backup.dump(2) << '\n';
        output.flush();
        if (!output) {
            throw std::runtime_error("Could not persist original zone activation bytes");
        }
    }
    {
        std::ifstream confirmation(staged_backup);
        if (Json::parse(confirmation) != backup) {
            throw std::runtime_error("Original-zone backup failed readback");
        }
    }
    if (!MoveFileExW(staged_backup.c_str(), backup_path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Could not atomically preserve original zone activation bytes");
    }

    bool reset = false;
    std::size_t remaining = 0;
    try {
        unlocker.apply(targets);
        reset = unlocker.clear_inactive_anchor();
        remaining = unlocker.plan_all_inactive().size();
        if (remaining != 0) {
            throw std::runtime_error("Final zone catalog verification failed");
        }
    } catch (...) {
        unlocker.restore(targets);
        throw;
    }
    std::cout << "Unlocked " << newly_inactive << " restricted zones using user mode; remaining="
              << remaining << "; inactive_anchor_reset=" << reset << ".\n";
    return remaining == 0 ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    std::ostringstream captured_output;
    auto *original_output = std::cout.rdbuf(captured_output.rdbuf());
    int result = 1;
    std::string error_message;
    try {
        result = run(argc, argv);
    } catch (const std::exception &error) {
        error_message = error.what();
    }
    std::cout.rdbuf(original_output);
    std::cout << captured_output.str();
    if (!error_message.empty()) {
        std::cerr << "Local zone unlock: " << error_message << '\n';
    }
    if (selected_pid != 0) {
        try {
            const auto directory = std::filesystem::current_path() / "out/launcher";
            std::filesystem::create_directories(directory);
            const auto report = directory /
                ("all-zones-result-" + std::to_string(selected_pid) + ".json");
            std::ofstream output(report, std::ios::trunc);
            output << Json{{"ok", result == 0}, {"pid", selected_pid}, {"mode", "usermode"},
                            {"detail", captured_output.str()}, {"error", error_message}}.dump(2)
                   << '\n';
            output.flush();
            if (!output) {
                throw std::runtime_error("Could not persist the zone helper result");
            }
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            result = 1;
        }
    }
    return result;
}
