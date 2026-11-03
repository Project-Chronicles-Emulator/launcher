#include "account_session.hpp"
#include "http_client.hpp"
#include "portal_feed.hpp"

#include <cstdlib>
#include <gtest/gtest.h>

namespace chronicles::launcher {
namespace {

nlohmann::json content_document() {
  return {{"store", nlohmann::json::array({{{"id", "collection"},
                                            {"published", true},
                                            {"sort_order", 0},
                                            {"title", "Collection"},
                                            {"summary", "See the shop"},
                                            {"category", "Outfit"},
                                            {"art", "citadel"},
                                            {"price_label", "View shop"},
                                            {"url", "https://shop.example/collection"}}})},
          {"community", nlohmann::json::array()}};
}

TEST(PortalFeedTest, OpensOnlyValidatedPublishedLinksAndClearsOnOriginChanges) {
  bool offline = false;
  PortalFeed feed([&](const std::string &) {
    if (offline) {
      throw std::runtime_error("offline");
    }
    return content_document();
  });
  feed.refresh("http://127.0.0.1:8480");
  EXPECT_EQ(feed.link("store", "collection"), "https://shop.example/collection");
  EXPECT_THROW(feed.link("store", "missing"), std::invalid_argument);
  offline = true;
  feed.refresh("http://127.0.0.1:8480", true);
  EXPECT_EQ(feed.state().at("status"), "error");
  EXPECT_EQ(feed.link("store", "collection"), "https://shop.example/collection");
  feed.refresh("https://other.example");
  EXPECT_TRUE(feed.state().at("store").empty());
  EXPECT_THROW(feed.link("store", "collection"), std::invalid_argument);
}

TEST(PortalFeedTest, RejectsUnsafeLinkWithoutReplacingLastGoodCatalog) {
  auto document = content_document();
  PortalFeed feed([&](const std::string &) { return document; });
  feed.refresh("http://127.0.0.1");
  document["store"][0]["url"] = "javascript:alert(1)";
  feed.refresh("http://127.0.0.1", true);
  EXPECT_EQ(feed.state().at("status"), "error");
  EXPECT_EQ(feed.link("store", "collection"), "https://shop.example/collection");
}

nlohmann::json roster_document() {
  return {{"characters", nlohmann::json::array({{{"id", "1"},
                                                 {"slot", 0},
                                                 {"name", "AliceHero"},
                                                 {"class_id", 1},
                                                 {"class_name", "Fight"},
                                                 {"level", 12},
                                                 {"experience", "2500"},
                                                 {"world_id", 1},
                                                 {"world_name", "Chronicles"},
                                                 {"equipment", nlohmann::json::array()}}})}};
}

TEST(LauncherRosterSessionTest, RefreshIsAuthenticatedAndClearsPrivateRosterOnSignOutOrExpiry) {
  unsigned roster_status = 0;
  auto document = roster_document();
  AccountSession session([&](const auto &method, const auto &url, const auto &, const auto &token) {
    if (url.ends_with("/login")) {
      return nlohmann::json{{"access_token", std::string(64, 'a')}};
    }
    if (url.ends_with("/characters")) {
      EXPECT_EQ(method, "GET");
      EXPECT_EQ(token, std::string(64, 'a'));
      if (roster_status) {
        throw HttpError("synthetic", roster_status);
      }
      return document;
    }
    return nlohmann::json::object();
  });
  session.sign_in("http://127.0.0.1", "alice", "synthetic-password");
  session.refresh_characters();
  EXPECT_EQ(session.state().at("characters").at("entries").size(), 1);
  document["characters"][0]["slot"] = 99;
  session.refresh_characters(true);
  EXPECT_EQ(session.state().at("characters").at("status"), "error");
  EXPECT_EQ(session.state().at("characters").at("entries").size(), 1);
  roster_status = 401;
  session.refresh_characters(true);
  EXPECT_EQ(session.state().at("status"), "expired");
  EXPECT_TRUE(session.state().at("characters").at("entries").empty());
  roster_status = 0;
  document = roster_document();
  session.sign_in("http://127.0.0.1", "alice", "synthetic-password");
  session.refresh_characters();
  session.sign_out();
  EXPECT_TRUE(session.state().at("characters").at("entries").empty());
}

TEST(PortalFeedIntegration, ReadsCatalogAndRosterThroughNativeTransport) {
  const auto *origin = std::getenv("CHRONICLES_PORTAL_TEST_API");
  if (!origin) {
    GTEST_SKIP() << "Run tools/portal_smoke_test.py with --launcher-tests";
  }
  PortalFeed content;
  content.refresh(origin);
  ASSERT_EQ(content.state().at("status"), "ready");
  ASSERT_EQ(content.state().at("store").size(), 1);
  EXPECT_EQ(content.link("store", "collection"), "https://shop.example/collection");
  AccountSession account;
  account.sign_in(origin, "portal_fixture", "synthetic-portal-password");
  account.refresh_characters();
  ASSERT_EQ(account.state().at("characters").at("status"), "ready");
  ASSERT_EQ(account.state().at("characters").at("entries").size(), 1);
  EXPECT_EQ(account.state().at("characters").at("entries").front().at("name"), "PortalHero");
}

} // namespace
} // namespace chronicles::launcher
