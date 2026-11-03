#pragma once

#include "process_memory.hpp"

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace chronicles::launcher {

struct ZoneUnlockRequest {
    std::uint16_t id{};
    std::string key;
};

struct ZoneUnlockTarget {
    ZoneUnlockRequest zone;
    std::uint64_t row_address{};
    std::uint64_t key_value{};
    std::uint8_t original_activation{};
    std::uint8_t original_field_access{};
};

// Local exploration of demo zones. This changes catalog data for the
// lifetime of one client, not collision, quest progress, map authority or executable code.
// Layout and signatures belong to client build 40367e24241c9481bfc07fbf9ef1919f25fb0eaa.
class ZoneUnlocker {
public:
    explicit ZoneUnlocker(ProcessMemory &memory,
                          std::chrono::milliseconds pending_stale_after = std::chrono::seconds(20));

    std::vector<ZoneUnlockTarget> plan(std::span<const ZoneUnlockRequest> requests);

    std::vector<ZoneUnlockTarget> plan_all_inactive();

    // Clears only the transient inactive-zone anchor; the game refreshes its movement factor.
    bool clear_inactive_anchor();

    // The client never times out a refused PC2S_ZoneChannelMoveReq, so it keeps the map-move
    // manager's pending transition and blocks movement at that boundary indefinitely. Clear the
    // pending request state only after it stayed set for pending_stale_after, so an accepted
    // handoff keeps its normal lifecycle. Returns true when a stale request was cleared.
    bool clear_stale_pending_transition();

    // The caller must persist the plan with the client PID and creation time before applying it.
    void apply(std::span<const ZoneUnlockTarget> targets);

    void restore(std::span<const ZoneUnlockTarget> targets);

private:
    void verify_build();

    void verify_target(const ZoneUnlockTarget &target, std::uint8_t expected_activation,
                       std::uint8_t expected_field_access);

    std::string read_name(std::uint64_t value);

    std::vector<ZoneUnlockTarget> inspect_catalog();

    std::uint64_t find_map_move_manager();

    ProcessMemory &memory_;
    std::chrono::milliseconds pending_stale_after_;
    std::uint64_t pending_manager_{};
    std::chrono::steady_clock::time_point pending_since_{};
};

} // namespace chronicles::launcher
