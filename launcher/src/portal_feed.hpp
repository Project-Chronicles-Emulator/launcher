#pragma once

#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace chronicles::launcher {

class PortalFeed {
public:
  using Transport = std::function<nlohmann::json(const std::string &)>;

  explicit PortalFeed(Transport transport = {});

  void refresh(const std::string &origin, bool force = false);

  nlohmann::json state() const;

  std::string link(const std::string &kind, const std::string &id) const;

private:
  Transport transport_;
  std::string origin_;
  std::string status_{"loading"};
  std::string error_;
  nlohmann::json store_ = nlohmann::json::array();
  nlohmann::json community_ = nlohmann::json::array();
  std::chrono::steady_clock::time_point next_refresh_{};
};

} // namespace chronicles::launcher
