#include "auth_redirect.hpp"
#include "encoding.hpp"
#include "fake_process_memory.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace chronicles::launcher {
namespace {

using test_support::FakeMemory;

struct ClientFixture {
  FakeMemory memory;
  std::uint64_t manager{0x400000};
  std::uint64_t region_array{0x500000};
  std::vector<std::uint64_t> region_strings;
  std::uint64_t current_string{0x600000};
  std::uint64_t selected_string{0x601000};

  ClientFixture() {
    memory.allocate(manager, 512);
    memory.allocate(region_array, 3 * 80);

    add_region(0, "Asia", "https://auth-kr-01.archeagechronicles.xlgames.com:38081", 0x610000);
    add_region(1, "EU-West", "https://auth-de-01.archeagechronicles.xlgames.com:38081", 0x611000);
    add_region(2, "NA", "https://auth-use-01.archeagechronicles.xlgames.com:38081", 0x612000);

    memory.write_u64(manager + 24, region_array);
    memory.write_u32(manager + 32, 3);
    memory.write_u32(manager + 36, 3);
    memory.write_u64(manager + 120, selected_string);
    memory.write_u32(manager + 128, 3);
    memory.write_u32(manager + 132, 8);
    memory.allocate_string(current_string,
                           u"https://auth-use-01.archeagechronicles.xlgames.com:38081", 64);
    memory.write_u64(manager + 208, current_string);
    memory.write_u32(manager + 216, 57);
    memory.write_u32(manager + 220, 64);

    memory.allocate_string(selected_string, u"NA", 8);

    // A copy of the bootstrap JSON also contains the official endpoints. Discovery must skip it
    // because no region FString header owns its buffer.
    memory.allocate_string(
        0x700000,
        u"{\"regions\":[{\"name\":\"NA\",\"auth_address\":\"https://auth-use-01.archeagechronicles"
        u".xlgames.com:38081\"}]}",
        512);
  }

  void add_region(int index, std::string_view name, std::string_view address,
                  std::uint64_t address_buffer) {
    const auto text = ascii_to_utf16(address);
    const auto count = static_cast<std::int32_t>(text.size() + 1);
    const auto capacity = 64;
    memory.allocate_string(address_buffer, text, capacity);
    region_strings.push_back(address_buffer);

    const auto name_buffer = 0x620000 + static_cast<std::uint64_t>(index) * 0x1000;
    memory.allocate_string(name_buffer, ascii_to_utf16(name), 8);

    const auto entry = region_array + static_cast<std::uint64_t>(index) * 80;
    memory.write_u64(entry, name_buffer);
    memory.write_u32(entry + 8, static_cast<std::uint32_t>(name.size() + 1));
    memory.write_u32(entry + 12, 8);
    memory.write_u64(entry + 16, address_buffer);
    memory.write_u32(entry + 24, static_cast<std::uint32_t>(count));
    memory.write_u32(entry + 28, capacity);
  }
};

TEST(Encoding, Base64RoundTripsEveryRemainder) {
  for (std::size_t length = 0; length < 64; ++length) {
    std::vector<std::uint8_t> bytes(length);
    for (std::size_t index = 0; index < length; ++index) {
      bytes[index] = static_cast<std::uint8_t>(index * 37 + length);
    }
    EXPECT_EQ(base64_decode(base64_encode(bytes)), bytes);
  }
}

TEST(Encoding, AddressFormattingMatchesBridgeContract) {
  EXPECT_EQ(format_address(0x1B818FB0), "0x1B818FB0");
  EXPECT_EQ(parse_address("0x1B818FB0"), 0x1B818FB0);
  EXPECT_THROW(parse_address("not-an-address"), std::invalid_argument);
}

TEST(Encoding, EndpointClassificationRequiresOfficialSuffixAndScheme) {
  EXPECT_TRUE(is_official_auth_address(
      ascii_to_utf16("https://auth-use-01.archeagechronicles.xlgames.com:38081")));
  EXPECT_TRUE(is_official_auth_address(
      ascii_to_utf16("http://auth-kr-01.archeagechronicles.xlgames.com:38081")));
  EXPECT_FALSE(is_official_auth_address(ascii_to_utf16("http://127.0.0.1:8080")));
  EXPECT_FALSE(is_official_auth_address(
      ascii_to_utf16("https://example.com/.archeagechronicles.xlgames.com")));
  EXPECT_FALSE(is_official_auth_address(
      ascii_to_utf16("file://auth-use-01.archeagechronicles.xlgames.com")));
}

TEST(AuthRedirect, DiscoversManagerFromAnchoredRegionString) {
  ClientFixture fixture;
  AuthRedirector redirector(fixture.memory, "http://127.0.0.1:8080");

  const auto manager = redirector.discover_manager({}, {});
  ASSERT_TRUE(manager);
  EXPECT_EQ(*manager, fixture.manager);

  const auto state = redirector.inspect(*manager);
  EXPECT_EQ(state.selected_region, "NA");
  ASSERT_EQ(state.regions.size(), 3);
  EXPECT_EQ(state.regions[2].name, "NA");
  EXPECT_FALSE(state.current_redirected);
}

TEST(AuthRedirect, DiscoversSingleRegionManagerWithMatchingSelection) {
  ClientFixture fixture;
  fixture.memory.write_u32(fixture.manager + 32, 1);
  fixture.memory.write_u32(fixture.manager + 36, 1);
  fixture.memory.allocate_string(fixture.selected_string, u"Asia", 8);
  fixture.memory.write_u32(fixture.manager + 128, 5);

  AuthRedirector redirector(fixture.memory, "http://127.0.0.1:8080");
  const auto manager = redirector.discover_manager({}, {});
  ASSERT_TRUE(manager.has_value());
  EXPECT_EQ(*manager, fixture.manager);
  EXPECT_TRUE(redirector.is_manager_valid(*manager));

  const auto state = redirector.inspect(*manager);
  ASSERT_EQ(state.regions.size(), 1);
  EXPECT_EQ(state.regions.front().name, "Asia");
}

TEST(AuthRedirect, AppliesEveryRegionAndCurrentAddressAndRestoresExactly) {
  ClientFixture fixture;
  AuthRedirector redirector(fixture.memory, "http://127.0.0.1:8080");
  const auto manager = redirector.discover_manager({}, {});
  ASSERT_TRUE(manager);

  const auto applied = redirector.apply(*manager);
  EXPECT_TRUE(applied.failures.empty());
  EXPECT_EQ(applied.patched.size(), 4);

  const auto redirected = redirector.inspect(*manager);
  EXPECT_EQ(redirected.current_address, "http://127.0.0.1:8080");
  EXPECT_TRUE(redirected.current_redirected);
  for (const auto &region : redirected.regions) {
    EXPECT_EQ(region.address, "http://127.0.0.1:8080");
    EXPECT_TRUE(region.redirected);
  }

  // The restored state must be byte-identical to the original official configuration.
  const auto restored = redirector.restore(applied.patched);
  EXPECT_EQ(restored.restored, 4);
  EXPECT_TRUE(restored.skipped.empty());

  const auto original = redirector.inspect(*manager);
  EXPECT_EQ(original.selected_region, "NA");
  EXPECT_EQ(original.current_address, "https://auth-use-01.archeagechronicles.xlgames.com:38081");
  for (const auto &region : original.regions) {
    EXPECT_FALSE(region.redirected);
  }
}

TEST(AuthRedirect, ApplyIsIdempotentAndRediscoveryWorksAfterPatching) {
  ClientFixture fixture;
  AuthRedirector redirector(fixture.memory, "http://127.0.0.1:8080");
  const auto manager = redirector.discover_manager({}, {});
  ASSERT_TRUE(manager);

  const auto first = redirector.apply(*manager);
  const auto second = redirector.apply(*manager);
  EXPECT_TRUE(second.patched.empty());
  EXPECT_TRUE(second.failures.empty());

  const auto rediscovered = redirector.discover_manager({}, {});
  ASSERT_TRUE(rediscovered);
  EXPECT_EQ(*rediscovered, *manager);

  const auto applied = redirector.apply(*manager);
  EXPECT_EQ(applied.patched.size(), 0);
}

TEST(AuthRedirect, CapacityGuardRejectsEveryTargetWhenReplacementIsTooLong) {
  ClientFixture fixture;
  const std::string long_replacement =
      "http://127.0.0.1:8080/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  AuthRedirector redirector(fixture.memory, long_replacement);
  const auto manager = redirector.discover_manager({}, {});
  ASSERT_TRUE(manager);

  const auto applied = redirector.apply(*manager);
  EXPECT_TRUE(applied.patched.empty());
  ASSERT_EQ(applied.failures.size(), 4);
  EXPECT_NE(applied.failures.front().find("too small"), std::string::npos);

  // Nothing may be written when the capacity check fails.
  const auto state = redirector.inspect(*manager);
  for (const auto &region : state.regions) {
    EXPECT_FALSE(region.redirected);
  }
}

TEST(AuthRedirect, RestoreSkipsTextChangedBySomeoneElse) {
  ClientFixture fixture;
  AuthRedirector redirector(fixture.memory, "http://127.0.0.1:8080");
  const auto manager = redirector.discover_manager({}, {});
  ASSERT_TRUE(manager);
  const auto applied = redirector.apply(*manager);

  const auto &target = applied.patched.front();
  const auto other_text = utf16_to_bytes(u"http://127.0.0.1:9999", true);
  fixture.memory.write(target.buffer_address, other_text);

  const auto restored = redirector.restore(applied.patched);
  EXPECT_EQ(restored.restored, 3);
  ASSERT_EQ(restored.skipped.size(), 1);
  EXPECT_NE(restored.skipped.front().find("no longer"), std::string::npos);
}

TEST(AuthRedirect, SharedRegionAndCurrentBufferIsPatchedOnce) {
  ClientFixture fixture;
  fixture.memory.write_u64(fixture.manager + 208, fixture.region_strings[2]);

  AuthRedirector redirector(fixture.memory, "http://127.0.0.1:8080");
  const auto manager = redirector.discover_manager({}, {});
  ASSERT_TRUE(manager);

  const auto applied = redirector.apply(*manager);
  EXPECT_TRUE(applied.failures.empty());
  EXPECT_EQ(applied.patched.size(), 3);

  const auto restored = redirector.restore(applied.patched);
  EXPECT_EQ(restored.restored, 3);
  EXPECT_TRUE(restored.skipped.empty());
  EXPECT_EQ(redirector.inspect(*manager).current_address,
            "https://auth-use-01.archeagechronicles.xlgames.com:38081");
}

TEST(AuthRedirect, DiscoveryAcceptsCurrentAddressClearedByClientReset) {
  ClientFixture fixture;
  fixture.memory.write_u64(fixture.manager + 208, 0);
  fixture.memory.write_u32(fixture.manager + 216, 0);
  fixture.memory.write_u32(fixture.manager + 220, 0);

  AuthRedirector redirector(fixture.memory, "http://127.0.0.1:8080");
  const auto manager = redirector.discover_manager({}, {});
  ASSERT_TRUE(manager);
  EXPECT_EQ(*manager, fixture.manager);

  const auto applied = redirector.apply(*manager);
  EXPECT_EQ(applied.patched.size(), 3);
  EXPECT_TRUE(applied.failures.empty());
  EXPECT_EQ(redirector.inspect(*manager).current_address, "");
}

TEST(AuthRedirect, CancelsLongDiscovery) {
  FakeMemory memory;
  AuthRedirector redirector(memory, "http://127.0.0.1:8080");

  std::stop_source source;
  source.request_stop();
  EXPECT_THROW(redirector.discover_manager(source.get_token(), {}), OperationCancelled);
}

} // namespace
} // namespace chronicles::launcher
