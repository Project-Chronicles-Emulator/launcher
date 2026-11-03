#include "zone_unlock.hpp"

#include "encoding.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>

namespace chronicles::launcher {
namespace {

constexpr std::uint64_t kCatalogOwnerGlobal = 0x14C3CF3D8;
constexpr std::uint64_t kNamePool = 0x14C121A00;
constexpr std::size_t kCatalogOffset = 96;
constexpr std::size_t kZoneTableOffset = 3024;
constexpr std::size_t kZoneEntrySize = 264;
constexpr std::size_t kZoneRowOffset = 8;
constexpr std::size_t kZoneRowSize = 248;
constexpr std::size_t kActivationOffset = 2;
// The zone lookup also gates exploration on this derived map-eligibility byte after checking
// activation. Setting activation alone leaves some root regions blocked in the demo.
constexpr std::size_t kFieldAccessOffset = 24;
constexpr std::size_t kRowKeyOffset = 200;
constexpr std::size_t kMaximumZoneEntries = 4096;

// X5MapMoveManager fields. The anchor arms the radial inactive-zone movement leash; the
// pending bytes hold a map handoff request the client waits on indefinitely when the server
// refuses it without a reply.
constexpr std::size_t kInactiveAnchorOffset = 432;
constexpr std::size_t kPendingTransitionOffset = 568;
constexpr std::size_t kPendingTransitionBytes = 3;

constexpr std::array<std::uint8_t, 24> kAnchorCode = {
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x80, 0xB9, 0xB0, 0x01, 0x00, 0x00,
    0x00, 0x48, 0x8B, 0xF9, 0x0F, 0x85, 0xB1, 0x00, 0x00, 0x00, 0x48, 0x89};

constexpr std::array<std::uint8_t, 24> kLimiterCode = {
    0x48, 0x83, 0xEC, 0x48, 0xF3, 0x0F, 0x10, 0x2D, 0x0C, 0x88, 0x64, 0x03,
    0x0F, 0x28, 0xE1, 0x0F, 0x29, 0x74, 0x24, 0x30, 0xF2, 0x41, 0x0F, 0x10};

std::vector<std::uint8_t> read_exact(ProcessMemory &memory, std::uint64_t address,
                                     std::size_t size) {
    auto bytes = memory.read(address, size);
    if (bytes.size() != size) {
        throw MemoryAccessError("Short read while inspecting the zone catalog");
    }
    return bytes;
}

std::uint64_t read_pointer(ProcessMemory &memory, std::uint64_t address) {
    const auto value = read_u64_le(read_exact(memory, address, 8), 0);
    if (value < 0x10000 || value >= 0x0000800000000000 || value % 8 != 0) {
        throw MemoryAccessError("Invalid zone catalog pointer");
    }
    return value;
}

std::uint16_t zone_id(std::span<const std::uint8_t> row) {
    return static_cast<std::uint16_t>(row[0] | (static_cast<unsigned>(row[1]) << 8));
}

void write_gate(ProcessMemory &memory, const ZoneUnlockTarget &target,
                std::size_t offset, std::uint8_t value) {
    const std::array<std::uint8_t, 1> bytes{value};
    const auto address = target.row_address + offset;
    memory.write(address, bytes);
    if (read_exact(memory, address, 1).front() != value) {
        throw MemoryAccessError("Zone gate write failed readback");
    }
}

} // namespace

ZoneUnlocker::ZoneUnlocker(ProcessMemory &memory, std::chrono::milliseconds pending_stale_after)
    : memory_(memory), pending_stale_after_(pending_stale_after) {
}

void ZoneUnlocker::verify_build() {
    if (!std::ranges::equal(read_exact(memory_, 0x145F04CB0, kAnchorCode.size()), kAnchorCode) ||
        !std::ranges::equal(read_exact(memory_, 0x145F05B50, kLimiterCode.size()), kLimiterCode)) {
        throw MemoryAccessError("Zone unlock does not support this client build");
    }
}

std::string ZoneUnlocker::read_name(std::uint64_t value) {
    const auto index = static_cast<std::uint32_t>(value);
    const auto number = static_cast<std::uint32_t>(value >> 32);
    const auto block = read_pointer(memory_, kNamePool + 8 * ((index >> 16) + 2ULL));
    const auto entry = block + 2 * (index & 0xFFFFULL);
    const auto header_bytes = read_exact(memory_, entry, 2);
    const auto header = zone_id(header_bytes);
    const auto length = header >> 6;
    if ((header & 1) != 0 || length == 0 || length > 128) {
        throw MemoryAccessError("Unexpected zone key encoding");
    }
    const auto bytes = read_exact(memory_, entry + 2, length);
    if (std::ranges::any_of(bytes, [](auto value) {
            return value < 32 || value > 126;
        })) {
        throw MemoryAccessError("Invalid zone key text");
    }
    std::string name(bytes.begin(), bytes.end());
    if (number != 0) {
        name += "_" + std::to_string(number - 1);
    }
    return name;
}

std::vector<ZoneUnlockTarget> ZoneUnlocker::plan(std::span<const ZoneUnlockRequest> requests) {
    verify_build();
    if (requests.empty() || requests.size() > kMaximumZoneEntries) {
        throw std::invalid_argument("Select a bounded list of local exploration zones");
    }

    std::set<std::uint16_t> requested_ids;
    for (const auto &request : requests) {
        if (request.id == 0 || request.key.empty() || !requested_ids.insert(request.id).second) {
            throw std::invalid_argument("Invalid or duplicate local zone selection");
        }
    }

    const auto catalog_targets = inspect_catalog();
    std::vector<ZoneUnlockTarget> targets;
    for (const auto &target : catalog_targets) {
        const auto request = std::ranges::find(requests, target.zone.id, &ZoneUnlockRequest::id);
        if (request == requests.end()) {
            continue;
        }
        if (target.zone.key != request->key) {
            throw MemoryAccessError("Selected zone does not match the researched catalog");
        }
        targets.push_back(target);
    }
    if (targets.size() != requested_ids.size()) {
        throw MemoryAccessError("A selected exploration zone is absent from the client catalog");
    }
    return targets;
}

std::vector<ZoneUnlockTarget> ZoneUnlocker::inspect_catalog() {
    verify_build();
    const auto owner = read_pointer(memory_, kCatalogOwnerGlobal);
    const auto catalog = read_pointer(memory_, owner + kCatalogOffset);
    const auto table = read_pointer(memory_, catalog + kZoneTableOffset);
    const auto header = read_exact(memory_, table, 80);
    const auto count = read_u32_le(header, 8);
    // This researched catalog has no removed sparse-map entries. Fail closed if that changes.
    if (count == 0 || count > kMaximumZoneEntries || read_u32_le(header, 52) != 0) {
        throw MemoryAccessError("Unexpected zone table allocation");
    }
    const auto data = read_pointer(memory_, table);
    const auto entries = read_exact(memory_, data, count * kZoneEntrySize);

    std::vector<ZoneUnlockTarget> targets;
    std::set<std::uint16_t> found_ids;
    for (std::size_t index = 0; index < count; ++index) {
        const auto offset = index * kZoneEntrySize;
        const auto row = std::span(entries).subspan(offset + kZoneRowOffset, kZoneRowSize);
        const auto id = zone_id(row);
        const auto key = read_u64_le(entries, offset);
        if (id == 0 || !found_ids.insert(id).second || row[kActivationOffset] > 1 ||
            row[kFieldAccessOffset] > 1 ||
            read_u64_le(row, kRowKeyOffset) != key) {
            throw MemoryAccessError("Zone row " + std::to_string(index) +
                " does not match the researched catalog layout (id=" + std::to_string(id) +
                ", activation=" + std::to_string(row[kActivationOffset]) +
                ", eligibility=" + std::to_string(row[kFieldAccessOffset]) + ")");
        }
        const ZoneUnlockRequest zone{id, read_name(key)};
        targets.push_back({zone, data + offset + kZoneRowOffset, key,
                           row[kActivationOffset], row[kFieldAccessOffset]});
    }
    return targets;
}

std::vector<ZoneUnlockTarget> ZoneUnlocker::plan_all_inactive() {
    auto targets = inspect_catalog();
    std::erase_if(targets, [](const auto &target) {
        return target.original_activation == 1 && target.original_field_access == 1;
    });
    return targets;
}

std::uint64_t ZoneUnlocker::find_map_move_manager() {
    verify_build();
    const auto engine_value = read_u64_le(read_exact(memory_, 0x14C380590, 8), 0);
    if (engine_value == 0) {
        return 0;
    }
    const auto engine = read_pointer(memory_, 0x14C380590);
    const auto contexts = read_exact(memory_, engine + 4544, 16);
    const auto context_count = read_u32_le(contexts, 8);
    if (context_count == 0 || context_count > 64) {
        return 0;
    }
    const auto context_data = read_pointer(memory_, engine + 4544);
    const auto context_array = read_exact(memory_, context_data, context_count * 8);
    for (std::size_t index = 0; index < context_count; ++index) {
        const auto context = read_u64_le(context_array, index * 8);
        if (read_exact(memory_, context, 1).front() != 1) {
            continue;
        }
        if (read_u64_le(read_exact(memory_, context + 520, 8), 0) == 0) {
            return 0;
        }
        const auto game = read_pointer(memory_, context + 520);
        const auto collection = read_exact(memory_, game + 264, 16);
        const auto count = read_u32_le(collection, 8);
        if (count == 0) {
            return 0;
        }
        if (count > 256) {
            throw MemoryAccessError("Unexpected game subsystem collection");
        }
        const auto subsystem_class = read_pointer(memory_, 0x14C458100);
        const auto subsystems = read_exact(memory_, read_pointer(memory_, game + 264), count * 24);
        for (std::size_t subsystem_index = 0; subsystem_index < count; ++subsystem_index) {
            const auto offset = subsystem_index * 24;
            if (read_u64_le(subsystems, offset) != subsystem_class) {
                continue;
            }
            const auto subsystem = read_u64_le(subsystems, offset + 8);
            const auto managers = read_exact(memory_, subsystem + 48, 16);
            const auto manager_count = read_u32_le(managers, 8);
            if (manager_count == 0) {
                return 0;
            }
            if (manager_count > 256) {
                throw MemoryAccessError("Unexpected game manager collection");
            }
            const auto entries = read_exact(memory_, read_pointer(memory_, subsystem + 48),
                                             manager_count * 24);
            for (std::size_t manager_index = 0; manager_index < manager_count; ++manager_index) {
                const auto manager_offset = manager_index * 24;
                if (read_name(read_u32_le(entries, manager_offset)) != "X5MapMoveManager") {
                    continue;
                }
                return read_u64_le(entries, manager_offset + 8);
            }
            return 0;
        }
    }
    return 0;
}

bool ZoneUnlocker::clear_inactive_anchor() {
    const auto manager = find_map_move_manager();
    if (manager == 0) {
        return false;
    }
    const auto active = read_exact(memory_, manager + kInactiveAnchorOffset, 1).front();
    if (active > 1) {
        throw MemoryAccessError("Invalid inactive-zone anchor flag");
    }
    if (active == 1) {
        const std::array<std::uint8_t, 1> cleared{0};
        memory_.write(manager + kInactiveAnchorOffset, cleared);
        if (read_exact(memory_, manager + kInactiveAnchorOffset, 1).front() != 0) {
            throw MemoryAccessError("Inactive-zone anchor reset failed readback");
        }
        return true;
    }
    return false;
}

bool ZoneUnlocker::clear_stale_pending_transition() {
    const auto manager = find_map_move_manager();
    if (manager == 0) {
        pending_manager_ = 0;
        return false;
    }
    if (read_exact(memory_, manager + kPendingTransitionOffset, 1).front() == 0) {
        pending_manager_ = 0;
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (manager != pending_manager_) {
        pending_manager_ = manager;
        pending_since_ = now;
        return false;
    }
    if (now - pending_since_ < pending_stale_after_) {
        return false;
    }

    const std::array<std::uint8_t, kPendingTransitionBytes> cleared{0, 0, 0};
    memory_.write(manager + kPendingTransitionOffset, cleared);
    if (read_exact(memory_, manager + kPendingTransitionOffset, 1).front() != 0) {
        throw MemoryAccessError("Pending map transition reset failed readback");
    }
    pending_manager_ = 0;
    return true;
}

void ZoneUnlocker::verify_target(const ZoneUnlockTarget &target,
                                  std::uint8_t expected_activation,
                                  std::uint8_t expected_field_access) {
    if (target.original_activation > 1 || expected_activation > 1 ||
        target.original_field_access > 1 || expected_field_access > 1) {
        throw MemoryAccessError("Invalid original zone activation value");
    }
    const auto row = read_exact(memory_, target.row_address, kZoneRowSize);
    if (zone_id(row) != target.zone.id || row[kActivationOffset] != expected_activation ||
        row[kFieldAccessOffset] != expected_field_access ||
        read_u64_le(row, kRowKeyOffset) != target.key_value ||
        read_name(target.key_value) != target.zone.key) {
        throw MemoryAccessError("Zone catalog changed since the original-byte backup");
    }
}

void ZoneUnlocker::apply(std::span<const ZoneUnlockTarget> targets) {
    verify_build();
    for (const auto &target : targets) {
        const auto row = read_exact(memory_, target.row_address, kZoneRowSize);
        verify_target(target, row[kActivationOffset], row[kFieldAccessOffset]);
    }
    std::size_t attempted = 0;
    try {
        for (const auto &target : targets) {
            ++attempted;
            if (read_exact(memory_, target.row_address + kActivationOffset, 1).front() == 0) {
                write_gate(memory_, target, kActivationOffset, 1);
            }
            if (read_exact(memory_, target.row_address + kFieldAccessOffset, 1).front() == 0) {
                write_gate(memory_, target, kFieldAccessOffset, 1);
            }
        }
    } catch (...) {
        // Include the last attempted write: a transport error can follow a successful mutation.
        restore(targets.first(attempted));
        throw;
    }
}

void ZoneUnlocker::restore(std::span<const ZoneUnlockTarget> targets) {
    verify_build();
    for (const auto &target : targets) {
        const auto row = read_exact(memory_, target.row_address, kZoneRowSize);
        verify_target(target, row[kActivationOffset], row[kFieldAccessOffset]);
        if (row[kActivationOffset] != target.original_activation) {
            write_gate(memory_, target, kActivationOffset, target.original_activation);
        }
        if (row[kFieldAccessOffset] != target.original_field_access) {
            write_gate(memory_, target, kFieldAccessOffset, target.original_field_access);
        }
    }
}

} // namespace chronicles::launcher
