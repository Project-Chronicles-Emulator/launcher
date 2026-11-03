#include "portal_feed.hpp"
#include "distribution/distribution.hpp"
#include "http_client.hpp"
#include "services/portal/portal_content.hpp"
#include "xorstr.hpp"

#include <stdexcept>

namespace chronicles::launcher {
namespace {

nlohmann::json validated_section(const nlohmann::json &document, const char *kind) {
  const auto &entries = document.at(kind);
  if (!entries.is_array() || entries.size() > 40) {
    throw std::invalid_argument("Invalid content feed size");
  }
  auto result = nlohmann::json::array();
  for (const auto &entry : entries) {
    auto normalized = validate_portal_entry(kind, entry);
    if (!normalized.at("published").get<bool>()) {
      throw std::invalid_argument("Public content contains a draft");
    }
    result.push_back(std::move(normalized));
  }
  return result;
}

} // namespace

PortalFeed::PortalFeed(Transport transport) : transport_(std::move(transport)) {
  if (!transport_) {
    transport_ = [](const std::string &url) { return http_get_json(url, 3000); };
  }
}

void PortalFeed::refresh(const std::string &origin, bool force) {
  const auto normalized = distribution::parse_service_origin(origin).url;
  if (normalized != origin_) {
    origin_ = normalized;
    store_ = nlohmann::json::array();
    community_ = nlohmann::json::array();
    next_refresh_ = {};
  }
  if (!force && std::chrono::steady_clock::now() < next_refresh_) {
    return;
  }
  try {
    const auto document = transport_(origin_ + xorstr_("/launcher/v1/content"));
    auto store = validated_section(document, "store");
    auto community = validated_section(document, "community");
    store_ = std::move(store);
    community_ = std::move(community);
    status_ = "ready";
    error_.clear();
    next_refresh_ = std::chrono::steady_clock::now() + std::chrono::minutes(5);
  } catch (const std::exception &) {
    status_ = "error";
    error_ = "Could not refresh content. Previously loaded links remain available.";
    next_refresh_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  }
}

nlohmann::json PortalFeed::state() const {
  return {{"status", status_}, {"error", error_}, {"store", store_}, {"community", community_}};
}

std::string PortalFeed::link(const std::string &kind, const std::string &id) const {
  validate_portal_kind(kind);
  validate_portal_id(id);
  const auto &entries = kind == "store" ? store_ : community_;
  for (const auto &entry : entries) {
    if (entry.at("id") == id) {
      const auto url = entry.at("url").get<std::string>();
      validate_portal_url(url);
      return url;
    }
  }
  throw std::invalid_argument("This link is no longer available. Refresh the page.");
}

} // namespace chronicles::launcher
