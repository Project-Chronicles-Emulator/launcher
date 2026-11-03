#pragma once

#include <memory>
#include <string>

namespace chronicles::launcher {

// Receives a one-use browser proof on an OS-assigned loopback port. It never
// receives the Discord app secret, Discord access token or Project Chronicles session token.
class DiscordLoopback {
public:
  DiscordLoopback();
  ~DiscordLoopback();

  unsigned short port() const;
  void expect_state(std::string state);
  std::string poll();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace chronicles::launcher
