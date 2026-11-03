#include "news_feed.hpp"

#include <cstdlib>
#include <gtest/gtest.h>

namespace chronicles::launcher {
namespace {

nlohmann::json feed() {
  return {{"articles", nlohmann::json::array({{{"id", "update"},
                                               {"category", "Launcher"},
                                               {"title", "An update"},
                                               {"summary", "Stored news"},
                                               {"body", "Plain text <script> is not HTML."},
                                               {"art", "citadel"},
                                               {"published_at", 1792800000}}})}};
}

TEST(NewsFeedTest, ThrottlesRefreshAndRetainsLastGoodFeedAfterFailure) {
  int calls = 0;
  NewsFeed news([&](const std::string &url) {
    EXPECT_EQ(url, "http://127.0.0.1:8480/launcher/v1/news");
    if (++calls > 1) {
      throw std::runtime_error("offline");
    }
    return feed();
  });
  news.refresh("http://127.0.0.1:8480");
  news.refresh("http://127.0.0.1:8480");
  EXPECT_EQ(calls, 1);
  news.refresh("http://127.0.0.1:8480", true);
  EXPECT_EQ(news.state().at("status"), "error");
  EXPECT_EQ(news.state().at("articles").size(), 1);
  EXPECT_GT(news.state().at("fetched_at").get<std::int64_t>(), 0);
}

TEST(NewsFeedTest, SwitchingServersClearsPreviousArticlesEvenIfNewServerIsOffline) {
  NewsFeed news([](const std::string &url) {
    if (url.starts_with("https://other.example")) {
      throw std::runtime_error("offline");
    }
    return feed();
  });
  news.refresh("http://127.0.0.1:8480");
  news.refresh("https://other.example");
  EXPECT_TRUE(news.state().at("articles").empty());
  EXPECT_EQ(news.state().at("fetched_at"), 0);
}

TEST(NewsFeedTest, RejectsMalformedFeedsAndAcceptsAnEmptyPublication) {
  auto document = feed();
  NewsFeed news([&](const std::string &) { return document; });
  document["articles"][0]["art"] = "remote-url";
  news.refresh("http://127.0.0.1");
  EXPECT_EQ(news.state().at("status"), "error");
  EXPECT_TRUE(news.state().at("articles").empty());
  document = feed();
  document["articles"][0]["published_at"] = 1.5;
  news.refresh("http://127.0.0.1", true);
  EXPECT_EQ(news.state().at("status"), "error");
  document["articles"] = nlohmann::json::array();
  news.refresh("http://127.0.0.1", true);
  EXPECT_EQ(news.state().at("status"), "ready");
  EXPECT_TRUE(news.state().at("articles").empty());
}

TEST(NewsFeedIntegration, ReadsPublishedNewsThroughWinHttp) {
  const auto *origin = std::getenv("CHRONICLES_NEWS_TEST_API");
  if (!origin) {
    GTEST_SKIP() << "Run tools/news_smoke_test.py with --launcher-tests";
  }
  NewsFeed news;
  news.refresh(origin);
  ASSERT_EQ(news.state().at("status"), "ready");
  ASSERT_EQ(news.state().at("articles").size(), 1);
  EXPECT_EQ(news.state().at("articles").front().at("id"), "launcher-news-test");
}

} // namespace
} // namespace chronicles::launcher
