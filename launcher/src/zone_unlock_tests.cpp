#include "zone_unlock.hpp"

#include "fake_process_memory.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <optional>
#include <vector>

namespace chronicles::launcher {
namespace {

constexpr std::uint64_t kOwner = 0x200000;
constexpr std::uint64_t kCatalog = 0x300000;
constexpr std::uint64_t kTable = 0x400000;
constexpr std::uint64_t kEntries = 0x500000;
constexpr std::uint64_t kNames = 0x600000;
constexpr std::uint64_t kHousingRow = kEntries + 264 + 8;

class ZoneMemory : public test_support::FakeMemory {
public:
    void write(std::uint64_t address, std::span<const std::uint8_t> bytes) override {
        test_support::FakeMemory::write(address, bytes);
        if (fail_after_write == address) {
            fail_after_write.reset();
            throw MemoryAccessError("Simulated lost response after committed write");
        }
    }

    std::optional<std::uint64_t> fail_after_write;
};

class ZoneUnlockTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::array<std::uint8_t, 24> anchor = {
            0x40, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x80, 0xB9, 0xB0, 0x01, 0, 0,
            0, 0x48, 0x8B, 0xF9, 0x0F, 0x85, 0xB1, 0, 0, 0, 0x48, 0x89};
        const std::array<std::uint8_t, 24> limiter = {
            0x48, 0x83, 0xEC, 0x48, 0xF3, 0x0F, 0x10, 0x2D, 0x0C, 0x88, 0x64, 0x03,
            0x0F, 0x28, 0xE1, 0x0F, 0x29, 0x74, 0x24, 0x30, 0xF2, 0x41, 0x0F, 0x10};
        for (const auto address : {0x145F04CB0ULL, 0x145F05B50ULL, 0x14C3CF3D8ULL,
                                  0x14C121A10ULL, kOwner, kCatalog, kTable, kEntries, kNames}) {
            memory.allocate(address, 4096);
        }
        memory.write(0x145F04CB0, anchor);
        memory.write(0x145F05B50, limiter);
        memory.write_u64(0x14C3CF3D8, kOwner);
        memory.write_u64(kOwner + 96, kCatalog);
        memory.write_u64(kCatalog + 3024, kTable);
        memory.write_u64(kTable, kEntries);
        memory.write_u32(kTable + 8, 3);
        memory.write_u64(0x14C121A10, kNames);
        add_zone(0, 100, 1, "Zone_01", 1);
        add_zone(1, 112, 32, "Zone_01_Housing_001", 0);
        add_zone(2, 113, 64, "OtherInactiveZone", 0);
        memory.write_count = 0;
    }

    void add_zone(std::size_t index, std::uint16_t id, std::uint32_t name_index,
                  std::string_view key, std::uint8_t active) {
        const auto entry = kEntries + index * 264;
        memory.write_u64(entry, name_index);
        memory.write_u32(entry + 8, id | (static_cast<unsigned>(active) << 16));
        memory.write_u64(entry + 8 + 200, name_index);
        memory.write_u32(entry + 8 + 24, active);
        const auto name = kNames + 2 * name_index;
        const auto header = static_cast<std::uint16_t>(key.size() << 6);
        const std::array<std::uint8_t, 2> header_bytes{
            static_cast<std::uint8_t>(header), static_cast<std::uint8_t>(header >> 8)};
        memory.write(name, header_bytes);
        memory.write(name + 2, std::span(reinterpret_cast<const std::uint8_t *>(key.data()),
                                        key.size()));
    }

    // Builds the live engine-to-manager chain and returns the X5MapMoveManager address.
    std::uint64_t add_world_chain() {
        constexpr std::uint64_t engine = 0x700000;
        constexpr std::uint64_t contexts = 0x710000;
        constexpr std::uint64_t context = 0x720000;
        constexpr std::uint64_t game = 0x730000;
        constexpr std::uint64_t subsystems = 0x740000;
        constexpr std::uint64_t subsystem = 0x750000;
        constexpr std::uint64_t managers = 0x760000;
        constexpr std::uint64_t manager = 0x770000;
        constexpr std::uint64_t subsystem_class = 0x780000;
        for (const auto address : {0x14C380590ULL, 0x14C458100ULL, contexts, context, game,
                                  subsystems, subsystem, managers, manager, subsystem_class}) {
            memory.allocate(address, 4096);
        }
        memory.allocate(engine, 8192);
        memory.write_u64(0x14C380590, engine);
        memory.write_u64(engine + 4544, contexts);
        memory.write_u32(engine + 4552, 1);
        memory.write_u64(contexts, context);
        memory.write_u32(context, 1);
        memory.write_u64(context + 520, game);
        memory.write_u64(game + 264, subsystems);
        memory.write_u32(game + 272, 1);
        memory.write_u64(0x14C458100, subsystem_class);
        memory.write_u64(subsystems, subsystem_class);
        memory.write_u64(subsystems + 8, subsystem);
        memory.write_u64(subsystem + 48, managers);
        memory.write_u32(subsystem + 56, 1);
        const std::string name = "X5MapMoveManager";
        const auto header = static_cast<std::uint16_t>(name.size() << 6);
        const std::array<std::uint8_t, 2> header_bytes{
            static_cast<std::uint8_t>(header), static_cast<std::uint8_t>(header >> 8)};
        memory.write(kNames + 192, header_bytes);
        memory.write(kNames + 194,
                     std::span(reinterpret_cast<const std::uint8_t *>(name.data()), name.size()));
        memory.write_u64(managers, 96);
        memory.write_u64(managers + 8, manager);
        return manager;
    }

    ZoneMemory memory;
    const std::array<ZoneUnlockRequest, 1> requests{
        ZoneUnlockRequest{112, "Zone_01_Housing_001"}};
};

TEST_F(ZoneUnlockTest, ChangesOnlySelectedActivationAndRestoresOriginalBytes) {
    const auto before = memory.read(kEntries, 3 * 264);
    ZoneUnlocker unlocker(memory);
    const auto plan = unlocker.plan(requests);
    ASSERT_EQ(plan.size(), 1);
    EXPECT_EQ(memory.write_count, 0);
    unlocker.apply(plan);

    auto expected = before;
    expected[264 + 10] = 1;
    expected[264 + 8 + 24] = 1;
    EXPECT_EQ(memory.read(kEntries, 3 * 264), expected);
    unlocker.restore(plan);
    EXPECT_EQ(memory.read(kEntries, 3 * 264), before);
}

TEST_F(ZoneUnlockTest, ReapplyingRetainsTheOriginalBackup) {
    ZoneUnlocker unlocker(memory);
    const auto plan = unlocker.plan(requests);
    unlocker.apply(plan);
    unlocker.apply(plan);
    EXPECT_EQ(memory.write_count, 2);
    unlocker.restore(plan);
    EXPECT_EQ(memory.read(kHousingRow + 2, 1).front(), 0);
}

TEST_F(ZoneUnlockTest, RejectsUnknownBuildWithoutWriting) {
    memory.write_u32(0x145F04CB0, 0);
    memory.write_count = 0;
    ZoneUnlocker unlocker(memory);
    EXPECT_THROW(unlocker.plan(requests), MemoryAccessError);
    EXPECT_EQ(memory.write_count, 0);
}

TEST_F(ZoneUnlockTest, RejectsMissingOrMismatchedZoneWithoutWriting) {
    ZoneUnlocker unlocker(memory);
    const std::array wrong_key{ZoneUnlockRequest{112, "OtherInactiveZone"}};
    const std::array missing{ZoneUnlockRequest{999, "Absent"}};
    EXPECT_THROW(unlocker.plan(wrong_key), MemoryAccessError);
    EXPECT_THROW(unlocker.plan(missing), MemoryAccessError);
    EXPECT_EQ(memory.write_count, 0);
}

TEST_F(ZoneUnlockTest, RejectsReusedRowBeforeWriting) {
    ZoneUnlocker unlocker(memory);
    const auto plan = unlocker.plan(requests);
    memory.write_u32(kHousingRow, 999);
    memory.write_count = 0;
    EXPECT_THROW(unlocker.apply(plan), MemoryAccessError);
    EXPECT_EQ(memory.write_count, 0);
}

TEST_F(ZoneUnlockTest, RollsBackACommittedWriteWhoseResponseWasLost) {
    ZoneUnlocker unlocker(memory);
    const auto plan = unlocker.plan(requests);
    memory.fail_after_write = kHousingRow + 2;
    EXPECT_THROW(unlocker.apply(plan), MemoryAccessError);
    EXPECT_EQ(memory.read(kHousingRow + 2, 1).front(), 0);
}

TEST_F(ZoneUnlockTest, RejectsUnexpectedSparseTableAndNonBooleanActivation) {
    ZoneUnlocker unlocker(memory);
    memory.write_u32(kTable + 52, 1);
    EXPECT_THROW(unlocker.plan(requests), MemoryAccessError);
    memory.write_u32(kTable + 52, 0);
    memory.write_u32(kHousingRow, 112 | (2 << 16));
    memory.write_count = 0;
    EXPECT_THROW(unlocker.plan(requests), MemoryAccessError);
    EXPECT_EQ(memory.write_count, 0);
}

TEST_F(ZoneUnlockTest, RejectsDuplicateRequests) {
    ZoneUnlocker unlocker(memory);
    const std::array duplicate{requests.front(), requests.front()};
    EXPECT_THROW(unlocker.plan(duplicate), std::invalid_argument);
    EXPECT_EQ(memory.write_count, 0);
}

TEST_F(ZoneUnlockTest, AllZoneSelectionIncludesEveryInactiveRegionAndPreservesActiveRows) {
    ZoneUnlocker unlocker(memory);
    const auto plan = unlocker.plan_all_inactive();
    ASSERT_EQ(plan.size(), 2);
    const auto active_before = memory.read(kEntries, 264);
    unlocker.apply(plan);
    EXPECT_TRUE(unlocker.plan_all_inactive().empty());
    EXPECT_EQ(memory.read(kEntries, 264), active_before);
    unlocker.restore(plan);
    EXPECT_EQ(unlocker.plan_all_inactive().size(), 2);
}

TEST_F(ZoneUnlockTest, AllZoneFailureRestoresEarlierWritesAndTheLastAttempt) {
    ZoneUnlocker unlocker(memory);
    const auto plan = unlocker.plan_all_inactive();
    const auto original = memory.read(kEntries, 3 * 264);
    memory.fail_after_write = kEntries + 2 * 264 + 10;
    EXPECT_THROW(unlocker.apply(plan), MemoryAccessError);
    EXPECT_EQ(memory.read(kEntries, 3 * 264), original);
}

TEST_F(ZoneUnlockTest, AllZoneSelectionAlsoUnlocksAnActiveButIneligibleRoot) {
    memory.write_u32(kEntries + 8 + 24, 0);
    ZoneUnlocker unlocker(memory);
    const auto plan = unlocker.plan_all_inactive();
    ASSERT_EQ(plan.size(), 3);
    unlocker.apply(plan);
    EXPECT_TRUE(unlocker.plan_all_inactive().empty());
    unlocker.restore(plan);
    EXPECT_EQ(memory.read(kEntries + 8 + 2, 1).front(), 1);
    EXPECT_EQ(memory.read(kEntries + 8 + 24, 1).front(), 0);
}

TEST_F(ZoneUnlockTest, EligibilityWriteFailureRestoresBothOriginalGateBytes) {
    ZoneUnlocker unlocker(memory);
    const auto plan = unlocker.plan(requests);
    const auto original = memory.read(kEntries, 3 * 264);
    memory.fail_after_write = kHousingRow + 24;
    EXPECT_THROW(unlocker.apply(plan), MemoryAccessError);
    EXPECT_EQ(memory.read(kEntries, 3 * 264), original);
}

TEST_F(ZoneUnlockTest, NoEngineBeforeWorldEntryDoesNotRequireAnAnchorReset) {
    memory.allocate(0x14C380590, 8);
    ZoneUnlocker unlocker(memory);
    EXPECT_FALSE(unlocker.clear_inactive_anchor());
}

TEST_F(ZoneUnlockTest, AcceptsUnrealNumberedZoneNames) {
    add_zone(2, 113, 64, "Zone", 0);
    const auto numbered_key = 64ULL | (12ULL << 32);
    memory.write_u64(kEntries + 2 * 264, numbered_key);
    memory.write_u64(kEntries + 2 * 264 + 8 + 200, numbered_key);
    const std::array numbered_request{ZoneUnlockRequest{113, "Zone_11"}};
    ZoneUnlocker unlocker(memory);
    const auto plan = unlocker.plan(numbered_request);
    ASSERT_EQ(plan.size(), 1);
    EXPECT_EQ(plan.front().zone.key, "Zone_11");
    unlocker.apply(plan);
    unlocker.restore(plan);
}

TEST_F(ZoneUnlockTest, ClearsOnlyTheInactiveAnchorAndPreservesPendingTravel) {
    const auto manager = add_world_chain();
    memory.write_u32(manager + 432, 1);
    memory.write_u32(manager + 568, 1);
    const auto before = memory.read(manager, 600);
    memory.write_count = 0;

    ZoneUnlocker unlocker(memory);
    EXPECT_TRUE(unlocker.clear_inactive_anchor());
    auto expected = before;
    expected[432] = 0;
    EXPECT_EQ(memory.read(manager, 600), expected);
    EXPECT_EQ(memory.write_count, 1);
}

TEST_F(ZoneUnlockTest, ClearsAStalePendingTransitionOnlyAfterTheGraceWindow) {
    const auto manager = add_world_chain();
    memory.write_u32(manager + 568, 0x01010101);
    memory.write_u32(manager + 572, 1);
    memory.write_count = 0;

    ZoneUnlocker unlocker(memory, std::chrono::milliseconds(0));
    EXPECT_FALSE(unlocker.clear_stale_pending_transition());
    EXPECT_EQ(memory.write_count, 0);

    EXPECT_TRUE(unlocker.clear_stale_pending_transition());
    EXPECT_EQ(memory.read(manager + 568, 3), (std::vector<std::uint8_t>{0, 0, 0}));
    EXPECT_EQ(memory.read(manager + 571, 1).front(), 1);
    EXPECT_EQ(memory.write_count, 1);
}

TEST_F(ZoneUnlockTest, KeepsAFreshPendingTransitionInsideTheGraceWindow) {
    const auto manager = add_world_chain();
    memory.write_u32(manager + 568, 1);
    const auto before = memory.read(manager, 600);
    memory.write_count = 0;

    ZoneUnlocker unlocker(memory);
    EXPECT_FALSE(unlocker.clear_stale_pending_transition());
    EXPECT_FALSE(unlocker.clear_stale_pending_transition());
    EXPECT_EQ(memory.read(manager, 600), before);
    EXPECT_EQ(memory.write_count, 0);
}

TEST_F(ZoneUnlockTest, ForgetsAPendingTransitionThatTheGameResolved) {
    const auto manager = add_world_chain();
    memory.write_u32(manager + 568, 1);
    ZoneUnlocker unlocker(memory, std::chrono::milliseconds(0));
    EXPECT_FALSE(unlocker.clear_stale_pending_transition());

    memory.write_u32(manager + 568, 0);
    EXPECT_FALSE(unlocker.clear_stale_pending_transition());

    memory.write_u32(manager + 568, 1);
    memory.write_count = 0;
    EXPECT_FALSE(unlocker.clear_stale_pending_transition());
    EXPECT_TRUE(unlocker.clear_stale_pending_transition());
    EXPECT_EQ(memory.write_count, 1);
}

} // namespace
} // namespace chronicles::launcher
