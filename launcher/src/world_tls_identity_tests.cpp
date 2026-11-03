#include "fake_process_memory.hpp"
#include "world_tls_identity.hpp"

#include <gtest/gtest.h>

#include <string>

namespace chronicles::launcher {
namespace {

class HostedTlsIdentity : public testing::Test {
protected:
  void SetUp() override {
    memory.allocate(kSsl, 0x100);
    memory.allocate(kParameters, 0x100);
    memory.allocate(kHosts, 0x100);
    memory.allocate(kHostData, 0x100);
    memory.allocate(kHostname, 0x100);
    memory.write_u64(kSsl + 0xD0, kParameters);
    memory.write_u64(kParameters + 0x30, kHosts);
    memory.write_u32(kParameters + 0x38, 4);
    memory.write_u32(kHosts, 1);
    memory.write_u64(kHosts + 8, kHostData);
    memory.write_u64(kHostData, kHostname);
    const std::string hostname = "world.chronicles.example";
    memory.write(kHostname, std::span(reinterpret_cast<const std::uint8_t *>(hostname.c_str()),
                                      hostname.size() + 1));
  }

  static constexpr std::uint64_t kSsl = 0x10000;
  static constexpr std::uint64_t kParameters = 0x20000;
  static constexpr std::uint64_t kHosts = 0x30000;
  static constexpr std::uint64_t kHostData = 0x40000;
  static constexpr std::uint64_t kHostname = 0x50000;
  test_support::FakeMemory memory;
};

TEST_F(HostedTlsIdentity, AcceptsSelectedDnsNameWithoutWritingMemory) {
  const auto writes_before = memory.write_count;
  EXPECT_NO_THROW(require_hosted_tls_identity(memory, kSsl, "WORLD.chronicles.example"));
  EXPECT_EQ(memory.write_count, writes_before);
}

TEST_F(HostedTlsIdentity, RejectsOtherHostnameAndPrefixMatches) {
  EXPECT_THROW(require_hosted_tls_identity(memory, kSsl, "other.chronicles.example"), std::runtime_error);
  EXPECT_THROW(require_hosted_tls_identity(memory, kSsl, "world.chronicles.invalid"), std::runtime_error);
}

TEST_F(HostedTlsIdentity, RejectsMissingIdentityEvenWhenSniCouldExist) {
  memory.write_u64(kParameters + 0x30, 0);
  EXPECT_THROW(require_hosted_tls_identity(memory, kSsl, "world.chronicles.example"), std::runtime_error);
}

TEST_F(HostedTlsIdentity, RejectsAlternativeHostList) {
  memory.write_u32(kHosts, 2);
  EXPECT_THROW(require_hosted_tls_identity(memory, kSsl, "world.chronicles.example"), std::runtime_error);
}

TEST_F(HostedTlsIdentity, RejectsVerificationWeakening) {
  for (const auto flags : {0x2u, 0x10u, 0x80000u, 0x200000u}) {
    memory.write_u32(kParameters + 0x14, flags);
    EXPECT_THROW(require_hosted_tls_identity(memory, kSsl, "world.chronicles.example"),
                 std::runtime_error);
  }
}

TEST_F(HostedTlsIdentity, RejectsLockedInheritanceAndBroadenedHostnameRules) {
  memory.write_u32(kParameters + 0x10, 8);
  EXPECT_THROW(require_hosted_tls_identity(memory, kSsl, "world.chronicles.example"), std::runtime_error);
  memory.write_u32(kParameters + 0x10, 0);
  memory.write_u32(kParameters + 0x38, 8);
  EXPECT_THROW(require_hosted_tls_identity(memory, kSsl, "world.chronicles.example"), std::runtime_error);
}

TEST_F(HostedTlsIdentity, RejectsUnreadableMemoryAndInvalidExpectedNames) {
  memory.write_u64(kHostData, 0x90000);
  EXPECT_THROW(require_hosted_tls_identity(memory, kSsl, "world.chronicles.example"), MemoryAccessError);
  EXPECT_THROW(require_hosted_tls_identity(memory, kSsl, ""), std::runtime_error);
  EXPECT_THROW(require_hosted_tls_identity(memory, kSsl, std::string_view("world\0other", 11)),
               std::runtime_error);
}

} // namespace
} // namespace chronicles::launcher
