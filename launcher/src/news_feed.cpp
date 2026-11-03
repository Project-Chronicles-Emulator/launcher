#include "news_feed.hpp"
#include "distribution/distribution.hpp"
#include "http_client.hpp"
#include "xorstr.hpp"

#include <stdexcept>

namespace chronicles::launcher {
namespace {

using json = nlohmann::json;

constexpr std::pair<const char *, std::size_t> kArticleFields[] = {
    {"id", 80}, {"category", 40}, {"title", 160}, {"summary", 400}, {"body", 16000}, {"art", 16}};

json validated_articles(const json &document) {
  const auto &articles = document.at("articles");
  if (!articles.is_array() || articles.size() > 40) {
    throw std::invalid_argument("Invalid news feed size");
  }
  auto result = json::array();
  for (const auto &article : articles) {
    json safe;
    for (const auto &[field, maximum] : kArticleFields) {
      const auto value = article.at(field).get<std::string>();
      if (value.empty() || value.size() > maximum || value.find('\0') != std::string::npos) {
        throw std::invalid_argument("Invalid news article field");
      }
      safe[field] = value;
    }
    const auto art = safe.at("art").get<std::string>();
    if (art != "citadel" && art != "voyage" && art != "company" && art != "discover") {
      throw std::invalid_argument("Invalid news artwork");
    }
    const auto &date = article.at("published_at");
    constexpr std::int64_t kMaximumTimestamp = 253402300799; // End of year 9999.
    if (!date.is_number_integer() ||
        (date.is_number_unsigned() && date.get<std::uint64_t>() > kMaximumTimestamp)) {
      throw std::invalid_argument("Invalid news publication date");
    }
    const auto published = date.get<std::int64_t>();
    if (published <= 0 || published > kMaximumTimestamp) {
      throw std::invalid_argument("Invalid news publication date");
    }
    safe["published_at"] = published;
    result.push_back(std::move(safe));
  }
  return result;
}

} // namespace

NewsFeed::NewsFeed(Transport transport) : transport_(std::move(transport)) {
  if (!transport_) {
    transport_ = [](const std::string &url) { return http_get_json(url, 3000); };
  }
}

void NewsFeed::refresh(const std::string &origin, bool force) {
  const auto normalized = distribution::parse_service_origin(origin).url;
  if (normalized != origin_) {
    // Never show one server's announcements after switching to another server.
    origin_ = normalized;
    articles_ = json::array();
    fetched_at_ = 0;
    next_refresh_ = {};
  }
  if (!force && std::chrono::steady_clock::now() < next_refresh_) {
    return;
  }
  try {
    auto articles = validated_articles(transport_(origin_ + xorstr_("/launcher/v1/news")));
    articles_ = std::move(articles);
    status_ = "ready";
    error_.clear();
    fetched_at_ = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    next_refresh_ = std::chrono::steady_clock::now() + std::chrono::minutes(5);
  } catch (const std::exception &) {
    status_ = "error";
    error_ = fetched_at_ == 0 ? "News is unavailable. Please try again."
                              : "Could not refresh news. Showing the last received articles.";
    next_refresh_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  }
}

json NewsFeed::state() const {
  return {
      {"status", status_}, {"articles", articles_}, {"error", error_}, {"fetched_at", fetched_at_}};
}

} // namespace chronicles::launcher
