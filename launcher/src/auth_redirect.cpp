#include "auth_redirect.hpp"
#include "xorstr.hpp"

#include "encoding.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

namespace chronicles::launcher {
namespace {

constexpr std::u16string_view kOfficialAddressSuffix = u".archeagechronicles.xlgames.com";

constexpr std::int32_t kMaximumAddressCharacters = 512;

constexpr std::int32_t kMaximumRegionNameCharacters = 64;

constexpr std::int32_t kMaximumRegions = 32;

constexpr int kDiscoveryAttempts = 6;

constexpr auto kDiscoveryRetryDelay = std::chrono::milliseconds(1500);

bool is_url_scheme(std::u16string_view text, std::u16string_view scheme) {
  return text.size() >= scheme.size() && text.substr(0, scheme.size()) == scheme;
}

bool is_domain_character(char16_t character) {
  const bool letter =
      (character >= u'a' && character <= u'z') || (character >= u'A' && character <= u'Z');
  const bool digit = character >= u'0' && character <= u'9';
  return letter || digit || character == u'-' || character == u'.';
}

} // namespace

bool is_official_auth_address(std::u16string_view address) {
  if (!is_url_scheme(address, u"http://") && !is_url_scheme(address, u"https://")) {
    return false;
  }

  const auto separator = address.find(u"://");
  auto authority = address.substr(separator + 3);
  const auto path = authority.find(u'/');
  if (path != std::u16string_view::npos) {
    authority = authority.substr(0, path);
  }

  const auto port = authority.rfind(u':');
  if (port != std::u16string_view::npos) {
    authority = authority.substr(0, port);
  }

  if (authority.empty() || authority.front() == u'.') {
    return false;
  }

  const auto suffix = kOfficialAddressSuffix;
  if (authority.size() <= suffix.size() ||
      authority.substr(authority.size() - suffix.size()) != suffix) {
    return false;
  }

  return std::ranges::all_of(authority, is_domain_character);
}

AuthRedirector::AuthRedirector(ProcessMemory &memory, std::string replacement_address,
                               AuthManagerLayout layout)
    : memory_(memory), replacement_address_(std::move(replacement_address)),
      replacement_text_(ascii_to_utf16(replacement_address_)), layout_(layout) {
  if (replacement_text_.empty() || (!is_url_scheme(replacement_text_, u"http://") &&
                                    !is_url_scheme(replacement_text_, u"https://"))) {
    throw std::invalid_argument("Replacement must be an http:// or https:// address");
  }
  if (replacement_text_.size() + 1 > static_cast<std::size_t>(kMaximumAddressCharacters)) {
    throw std::invalid_argument("Replacement address is too long");
  }
}

std::optional<std::uint64_t> AuthRedirector::discover_manager(std::stop_token stop,
                                                              const DiscoveryProgress &progress) {
  // Official region addresses start with the scheme at the first character of their FString
  // buffer, so these anchors identify buffer starts rather than positions inside a string.
  // The replacement is also tried: after a successful patch it is the only remaining anchor.
  const std::u16string official_secure = xorstr_(u"https://auth-");
  const std::u16string official_plain = xorstr_(u"http://auth-");
  const std::u16string_view anchors[] = {official_secure, official_plain, replacement_text_};

  for (int attempt = 0; attempt < kDiscoveryAttempts; ++attempt) {
    if (stop.stop_requested()) {
      throw OperationCancelled();
    }

    for (const auto anchor : anchors) {
      if (const auto manager = discover_with_anchor(anchor, stop, progress)) {
        return manager;
      }
    }

    if (attempt + 1 < kDiscoveryAttempts) {
      std::this_thread::sleep_for(kDiscoveryRetryDelay);
    }
  }

  return std::nullopt;
}

std::optional<std::uint64_t>
AuthRedirector::discover_with_anchor(std::u16string_view anchor_text, std::stop_token stop,
                                     const DiscoveryProgress &progress) {
  if (stop.stop_requested()) {
    throw OperationCancelled();
  }

  if (progress) {
    progress(DiscoveryPhase::FindingAddressText,
             "Scanning writable memory for the client's region endpoints");
  }

  const auto anchors = memory_.find_utf16(anchor_text, stop);
  if (progress) {
    progress(DiscoveryPhase::FindingRegionEntry,
             "Checking " + std::to_string(anchors.size()) + " endpoint strings for region entries");
  }

  for (const auto anchor : anchors) {
    if (stop.stop_requested()) {
      throw OperationCancelled();
    }

    const auto entry = find_entry_for_anchor(anchor, stop);
    if (!entry) {
      continue;
    }

    const auto first_entry = find_first_region_entry(*entry);
    if (first_entry == 0) {
      continue;
    }

    if (progress) {
      progress(DiscoveryPhase::FindingManager,
               "Region list found at " + format_address(first_entry) + "; locating its manager");
    }

    const auto manager = find_manager_for_regions(first_entry, stop);
    if (manager) {
      return manager;
    }
  }

  return std::nullopt;
}

bool AuthRedirector::is_manager_valid(std::uint64_t manager_address) {
  try {
    const auto regions = read_regions(manager_address);
    return !regions.empty();
  } catch (const MemoryAccessError &) {
    return false;
  }
}

AuthManagerState AuthRedirector::inspect(std::uint64_t manager_address) {
  AuthManagerState state;
  const auto regions = read_regions(manager_address);
  for (const auto &region : regions) {
    RegionState region_state;
    region_state.name = region.name;
    region_state.address = utf16_to_display_ascii(region.address);
    region_state.redirected = region.address == replacement_text_;
    state.regions.push_back(std::move(region_state));
  }

  const auto selected = read_fstring_text(
      memory_, read_array_header(memory_, manager_address + layout_.selected_region_offset),
      kMaximumRegionNameCharacters);
  if (selected) {
    state.selected_region = utf16_to_display_ascii(*selected);
  }

  const auto current = read_fstring_text(
      memory_, read_array_header(memory_, manager_address + layout_.current_address_offset),
      kMaximumAddressCharacters);
  if (current) {
    state.current_address = utf16_to_display_ascii(*current);
    state.current_redirected = *current == replacement_text_;
  }

  return state;
}

ApplyResult AuthRedirector::apply(std::uint64_t manager_address) {
  ApplyResult result;
  for (const auto &region : read_regions(manager_address)) {
    if (region.address == replacement_text_) {
      continue;
    }
    if (!is_official_auth_address(region.address)) {
      result.failures.push_back("Skipped unrecognized address for region " + region.name);
      continue;
    }

    try {
      result.patched.push_back(patch_address("region " + region.name,
                                             region.entry_address + layout_.region_address_offset,
                                             region.address_header, region.address));
    } catch (const std::exception &error) {
      result.failures.push_back("Region " + region.name + ": " + error.what());
    }
  }

  const auto current_header_address = manager_address + layout_.current_address_offset;
  const auto current_header = read_array_header(memory_, current_header_address);
  const auto current = read_fstring_text(memory_, current_header, kMaximumAddressCharacters);
  if (current && *current != replacement_text_ && is_official_auth_address(*current)) {
    try {
      result.patched.push_back(
          patch_address("current auth address", current_header_address, current_header, *current));
    } catch (const std::exception &error) {
      result.failures.push_back(std::string("Current auth address: ") + error.what());
    }
  }

  return result;
}

RestoreResult AuthRedirector::restore(std::span<const RedirectTarget> targets) {
  RestoreResult result;
  for (const auto &target : targets) {
    try {
      // Older backups lack the per-target replacement; they were all written with this one.
      const auto &replacement =
          target.replacement_address.empty() ? replacement_address_ : target.replacement_address;
      if (const auto skipped = restore_fstring(memory_, target, replacement)) {
        result.skipped.push_back(target.role + ": " + *skipped);
        continue;
      }
      ++result.restored;
    } catch (const std::exception &error) {
      result.skipped.push_back(target.role + ": " + error.what());
    }
  }

  return result;
}

const std::string &AuthRedirector::replacement_address() const {
  return replacement_address_;
}

std::optional<AuthRedirector::RegionEntry>
AuthRedirector::read_region_entry(std::uint64_t entry_address) {
  try {
    const auto name =
        read_short_ascii(read_array_header(memory_, entry_address + layout_.region_name_offset));
    if (!name) {
      return std::nullopt;
    }

    const auto address_header =
        read_array_header(memory_, entry_address + layout_.region_address_offset);
    const auto address = read_fstring_text(memory_, address_header, kMaximumAddressCharacters);
    if (!address || (!is_official_auth_address(*address) && *address != replacement_text_)) {
      return std::nullopt;
    }

    RegionEntry entry;
    entry.entry_address = entry_address;
    entry.name = *name;
    entry.address_header = address_header;
    entry.address = *address;
    return entry;
  } catch (const MemoryAccessError &) {
    // Scan hits include JSON copies, logs and freed buffers, not only live region entries.
    return std::nullopt;
  }
}

std::uint64_t AuthRedirector::find_first_region_entry(std::uint64_t entry_address) {
  // A bootstrap list is contiguous. Walk backwards while entries still look like regions, with a
  // hard bound so corrupt memory cannot turn this into an unbounded walk.
  for (int step = 0; step < kMaximumRegions; ++step) {
    const auto previous_address = entry_address - layout_.region_entry_size;
    try {
      if (!read_region_entry(previous_address)) {
        return entry_address;
      }
      entry_address = previous_address;
    } catch (const MemoryAccessError &) {
      return entry_address;
    }
  }

  return entry_address;
}

std::optional<std::uint64_t> AuthRedirector::find_entry_for_anchor(std::uint64_t anchor,
                                                                   std::stop_token stop) {
  // The anchor is expected at the first character of a URL buffer. Confirm the whole string is a
  // supported endpoint, then find the region entry whose FString header owns that buffer.
  std::optional<std::u16string> address;
  try {
    address = read_utf16_until_nul(memory_, anchor, kMaximumAddressCharacters);
  } catch (const MemoryAccessError &) {
    return std::nullopt;
  }
  if (!address || (!is_official_auth_address(*address) && *address != replacement_text_)) {
    return std::nullopt;
  }

  // A standalone FString allocation has allocator metadata or padding before it. If printable
  // text immediately precedes the scheme, this hit is embedded in a larger string (the bootstrap
  // JSON, a log line) and cannot be the region entry's own buffer. Skipping it here avoids a
  // full-process pointer scan for every JSON copy.
  try {
    const auto before = utf16_from_bytes(memory_.read(anchor - 16, 16));
    const auto previous = before.back();
    if (previous >= 0x20 && previous <= 0x7E) {
      return std::nullopt;
    }
  } catch (const std::exception &) {
    // If the prefix cannot be read, keep the candidate and let the structural checks decide.
  }

  const auto headers = memory_.find_pointer(anchor, stop);
  for (const auto header_address : headers) {
    if (stop.stop_requested()) {
      throw OperationCancelled();
    }
    if (header_address < layout_.region_address_offset) {
      continue;
    }

    const auto entry_address = header_address - layout_.region_address_offset;
    const auto entry = read_region_entry(entry_address);
    if (entry && entry->address_header.data_address == anchor) {
      return entry_address;
    }
  }

  return std::nullopt;
}

std::optional<std::uint64_t> AuthRedirector::find_manager_for_regions(std::uint64_t first_entry,
                                                                      std::stop_token stop) {
  const auto array_pointer_addresses = memory_.find_pointer(first_entry, stop);
  for (const auto pointer_address : array_pointer_addresses) {
    if (stop.stop_requested()) {
      throw OperationCancelled();
    }
    if (pointer_address < layout_.region_array_offset) {
      continue;
    }

    const auto manager_address = pointer_address - layout_.region_array_offset;
    try {
      const auto header = read_array_header(memory_, pointer_address);
      // A one-region array is structurally valid. Selection and current-address checks below
      // still have to validate the manager before any address is rewritten.
      if (header.data_address != first_entry || header.count < 1 ||
          header.count > kMaximumRegions || header.capacity < header.count) {
        continue;
      }

      const auto last_entry =
          first_entry + static_cast<std::uint64_t>(header.count - 1) * layout_.region_entry_size;
      const auto last_bytes =
          memory_.read(last_entry + layout_.region_address_offset, kArrayHeaderSize);
      if (parse_array_header(last_bytes).data_address == 0) {
        continue;
      }

      const auto regions = read_regions(manager_address);
      if (regions.size() != static_cast<std::size_t>(header.count)) {
        continue;
      }

      const auto selected = read_short_ascii(
          read_array_header(memory_, manager_address + layout_.selected_region_offset));
      const auto matches_selected =
          selected && std::ranges::any_of(regions, [&](const RegionEntry &region) {
            return region.name == *selected;
          });
      if (!matches_selected) {
        continue;
      }

      const auto current_header =
          read_array_header(memory_, manager_address + layout_.current_address_offset);
      // The client's logout/reset path clears this FString while retaining the region array and
      // selected region. An empty current address is therefore a valid discovery state.
      if (current_header.count != 0) {
        const auto current = read_fstring_text(memory_, current_header, kMaximumAddressCharacters);
        if (!current) {
          continue;
        }

        const bool known_current = is_official_auth_address(*current) ||
                                   *current == replacement_text_ ||
                                   std::ranges::any_of(regions, [&](const RegionEntry &region) {
                                     return region.address == *current;
                                   });
        if (!known_current) {
          continue;
        }
      }

      return manager_address;
    } catch (const MemoryAccessError &) {
      // A stale or partial candidate is not evidence of the manager.
    }
  }

  return std::nullopt;
}

std::vector<AuthRedirector::RegionEntry>
AuthRedirector::read_regions(std::uint64_t manager_address) {
  const auto array_header =
      read_array_header(memory_, manager_address + layout_.region_array_offset);
  if (array_header.count < 1 || array_header.count > kMaximumRegions) {
    throw MemoryAccessError("Region array has an implausible count");
  }

  std::vector<RegionEntry> regions;
  for (std::int32_t index = 0; index < array_header.count; ++index) {
    const auto entry_address =
        array_header.data_address + static_cast<std::uint64_t>(index) * layout_.region_entry_size;
    const auto entry = read_region_entry(entry_address);
    if (!entry) {
      throw MemoryAccessError("Region entry " + std::to_string(index) + " is not valid");
    }
    regions.push_back(*entry);
  }

  return regions;
}

std::optional<std::string> AuthRedirector::read_short_ascii(const ArrayHeader &header) {
  const auto text = read_fstring_text(memory_, header, kMaximumRegionNameCharacters);
  if (!text || text->empty()) {
    return std::nullopt;
  }

  const bool printable = std::ranges::all_of(
      *text, [](char16_t character) { return character >= 0x20 && character <= 0x7E; });
  if (!printable) {
    return std::nullopt;
  }

  return utf16_to_display_ascii(*text);
}

RedirectTarget AuthRedirector::patch_address(const std::string &role, std::uint64_t header_address,
                                             const ArrayHeader &header,
                                             const std::u16string &address) {
  return patch_fstring(memory_, role, header_address, header, address, replacement_address_,
                       kMaximumAddressCharacters);
}

} // namespace chronicles::launcher
