#pragma once

#include "fstring_patch.hpp"
#include "process_memory.hpp"
#include "unreal_string.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace chronicles::launcher {

// Field offsets inside the client's native authentication manager. They were recovered from
// client build hotfix-26.10.18.734 and observed live (docs/launcher.md). Every value read through
// them is validated by content before anything is written.
struct AuthManagerLayout {
  std::uint64_t region_array_offset{24};
  std::uint64_t selected_region_offset{120};
  std::uint64_t current_address_offset{208};
  std::uint64_t region_entry_size{80};
  std::uint64_t region_name_offset{0};
  std::uint64_t region_address_offset{16};
};

// True for the official regional authentication endpoints delivered by the bootstrap service,
// for example https://auth-use-01.archeagechronicles.xlgames.com:38081.
bool is_official_auth_address(std::u16string_view address);

struct RegionState {
  std::string name;
  std::string address;
  bool redirected{};
};

struct AuthManagerState {
  std::string selected_region;
  std::vector<RegionState> regions;
  std::string current_address;
  bool current_redirected{};
};

struct ApplyResult {
  std::vector<RedirectTarget> patched;
  std::vector<std::string> failures;
};

enum class DiscoveryPhase {
  FindingAddressText,
  FindingRegionEntry,
  FindingManager,
};

using DiscoveryProgress = std::function<void(DiscoveryPhase phase, const std::string &detail)>;

class AuthRedirector {
public:
  AuthRedirector(ProcessMemory &memory, std::string replacement_address,
                 AuthManagerLayout layout = {});

  // Locates the manager from its heap-allocated region list. Returns nullopt when the client has
  // not loaded the bootstrap region list yet. Official addresses are tried first; if the launcher
  // already redirected this client, the replacement address is used as the anchor instead.
  std::optional<std::uint64_t> discover_manager(std::stop_token stop,
                                                const DiscoveryProgress &progress);

  bool is_manager_valid(std::uint64_t manager_address);

  AuthManagerState inspect(std::uint64_t manager_address);

  // Rewrites every official address reachable from the manager. Already redirected strings are
  // left alone, so calling this repeatedly is safe and cheap.
  ApplyResult apply(std::uint64_t manager_address);

  RestoreResult restore(std::span<const RedirectTarget> targets);

  const std::string &replacement_address() const;

private:
  struct RegionEntry {
    std::uint64_t entry_address{};
    std::string name;
    ArrayHeader address_header;
    std::u16string address;
  };

  std::optional<RegionEntry> read_region_entry(std::uint64_t entry_address);

  std::uint64_t find_first_region_entry(std::uint64_t entry_address);

  // A scan hit points at the domain suffix inside a URL buffer. Resolve the buffer's first
  // character, then the region entry whose FString header owns that buffer.
  std::optional<std::uint64_t> find_entry_for_anchor(std::uint64_t anchor, std::stop_token stop);

  std::optional<std::uint64_t> find_manager_for_regions(std::uint64_t first_entry,
                                                        std::stop_token stop);

  std::optional<std::uint64_t> discover_with_anchor(std::u16string_view anchor_text,
                                                    std::stop_token stop,
                                                    const DiscoveryProgress &progress);

  std::vector<RegionEntry> read_regions(std::uint64_t manager_address);

  std::optional<std::string> read_short_ascii(const ArrayHeader &header);

  RedirectTarget patch_address(const std::string &role, std::uint64_t header_address,
                               const ArrayHeader &header, const std::u16string &address);

  ProcessMemory &memory_;
  std::string replacement_address_;
  std::u16string replacement_text_;
  AuthManagerLayout layout_;
};

} // namespace chronicles::launcher
