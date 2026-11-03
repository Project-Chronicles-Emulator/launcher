#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <nlohmann/json.hpp>

namespace chronicles::launcher {

class NewsFeed {
public:
  using Transport = std::function<nlohmann::json(const std::string &)>;

  explicit NewsFeed(Transport transport = {});

  void refresh(const std::string &origin, bool force = false);

  nlohmann::json state() const;

private:
  Transport transport_;
  std::string origin_;
  std::string status_{"loading"};
  std::string error_;
  nlohmann::json articles_ = nlohmann::json::array();
  std::chrono::steady_clock::time_point next_refresh_{};
  std::int64_t fetched_at_{};
};

} // namespace chronicles::launcher
