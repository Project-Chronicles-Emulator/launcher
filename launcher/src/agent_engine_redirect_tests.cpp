#include "agent_engine_redirect.hpp"
#include "encoding.hpp"
#include "fake_process_memory.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace chronicles::launcher {
namespace {

using test_support::FakeMemory;

// The front REST object keeps the agent-engine base URL at offset 0x168 (docs/agent-engine.md).
constexpr std::uint64_t kFrontRestObject = 0x400000;
constexpr std::uint64_t kBaseUrlHeader = kFrontRestObject + 0x168;
constexpr std::uint64_t kBaseUrlBuffer = 0x610000;
const std::int32_t kOfficialCount =
    static_cast<std::int32_t>(kOfficialAgentEngineAddress.size() + 1);
constexpr std::string_view kReplacement = "http://127.0.0.1:38011";

struct AgentEngineClient {
  FakeMemory memory;

  AgentEngineClient() {
    memory.allocate(kFrontRestObject, 0x200);
    memory.allocate_string(kBaseUrlBuffer, kOfficialAgentEngineAddress, kOfficialCount);
    memory.write_u64(kBaseUrlHeader, kBaseUrlBuffer);
    memory.write_u32(kBaseUrlHeader + 8, kOfficialCount);
    memory.write_u32(kBaseUrlHeader + 12, kOfficialCount);

    // The same text inside a longer string has no FString header of its own.
    memory.allocate_string(0x700000, u"base=http://modena.xlgames.corp:38011;other", 64);
    // A pointer to the buffer with a different length is not the base URL FString.
    memory.allocate(0x800000, 16);
    memory.write_u64(0x800000, kBaseUrlBuffer);
    memory.write_u32(0x800008, 5);
    memory.write_u32(0x80000C, kOfficialCount);
  }

  std::u16string base_url() {
    const auto header = read_array_header(memory, kBaseUrlHeader);
    return read_fstring_text(memory, header, 128).value_or(u"");
  }
};

TEST(AgentEngineRedirect, RewritesOnlyTheBaseUrlFString) {
  AgentEngineClient client;
  AgentEngineRedirector redirector(client.memory, std::string(kReplacement));

  const auto result = redirector.apply({});
  ASSERT_EQ(result.patched.size(), 1u);
  EXPECT_TRUE(result.failures.empty());
  EXPECT_EQ(result.patched.front().header_address, kBaseUrlHeader);
  EXPECT_EQ(result.patched.front().replacement_address, kReplacement);
  EXPECT_EQ(client.base_url(), ascii_to_utf16(kReplacement));

  // Neither the embedded copy nor the mismatched header changed.
  const auto embedded = read_utf16_until_nul(client.memory, 0x700000, 64);
  ASSERT_TRUE(embedded);
  EXPECT_NE(embedded->find(kOfficialAgentEngineAddress), std::u16string::npos);
  EXPECT_EQ(read_array_header(client.memory, 0x800000).count, 5);
}

TEST(AgentEngineRedirect, SecondPassReportsTheExistingRedirect) {
  AgentEngineClient client;
  AgentEngineRedirector redirector(client.memory, std::string(kReplacement));
  ASSERT_EQ(redirector.apply({}).patched.size(), 1u);

  const auto again = redirector.apply({});
  EXPECT_TRUE(again.patched.empty());
  EXPECT_EQ(again.already_redirected, 1u);
}

TEST(AgentEngineRedirect, RestorePutsTheOfficialAddressBack) {
  AgentEngineClient client;
  AgentEngineRedirector redirector(client.memory, std::string(kReplacement));
  const auto applied = redirector.apply({});
  ASSERT_EQ(applied.patched.size(), 1u);

  const auto restored = redirector.restore(applied.patched);
  EXPECT_EQ(restored.restored, 1);
  EXPECT_TRUE(restored.skipped.empty());
  EXPECT_EQ(client.base_url(), std::u16string(kOfficialAgentEngineAddress));
  EXPECT_EQ(read_array_header(client.memory, kBaseUrlHeader).count, kOfficialCount);
}

TEST(AgentEngineRedirect, RestoreLeavesAReassignedStringAlone) {
  AgentEngineClient client;
  AgentEngineRedirector redirector(client.memory, std::string(kReplacement));
  const auto applied = redirector.apply({});
  ASSERT_EQ(applied.patched.size(), 1u);

  client.memory.allocate_string(0x620000, u"http://other:1", 16);
  client.memory.write_u64(kBaseUrlHeader, 0x620000);
  const auto restored = redirector.restore(applied.patched);
  EXPECT_EQ(restored.restored, 0);
  ASSERT_EQ(restored.skipped.size(), 1u);
}

TEST(AgentEngineRedirect, RejectsAddressesThatCannotFitTheBuffer) {
  FakeMemory memory;
  EXPECT_THROW(AgentEngineRedirector(memory, "http://a-much-longer-host-name.example:38011"),
               std::invalid_argument);
  EXPECT_THROW(AgentEngineRedirector(memory, "https://127.0.0.1:38011"), std::invalid_argument);
}

TEST(AgentEngineRedirect, AddressUsesTheServiceHost) {
  EXPECT_EQ(agent_engine_address("http://127.0.0.1:8080", 38011), "http://127.0.0.1:38011");
}

} // namespace
} // namespace chronicles::launcher
