#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace chronicles::launcher {

struct ServerLaunchOptions {
  std::filesystem::path server_executable;
  std::filesystem::path project_root;
  std::filesystem::path run_directory;
  std::filesystem::path protocol_directory;
  std::filesystem::path client_data_directory;
  std::filesystem::path openssl_executable;
  std::string region;
  std::string account;
  int http_port{};
  int world_port{};
  // Agent engine (AI chat REST) listener; 0 leaves it disabled.
  int agent_engine_port{};
  bool world_enabled{};
  bool world_entry_diagnostic{};
};

// Runs chronicles-server as a hidden child with a disposable local account. The generated
// password exists only in the child's environment; it is never written to settings or logs.
class ServerProcess {
public:
  ServerProcess();
  ~ServerProcess();

  ServerProcess(const ServerProcess &) = delete;
  ServerProcess &operator=(const ServerProcess &) = delete;

  void start(const ServerLaunchOptions &options);

  // The emulator has no console control channel from a hidden window, so stopping terminates the
  // child. Memory-backed accounts and characters are disposable by design.
  void stop();

  bool is_running();

  std::uint32_t pid() const;

  std::string log_tail(std::size_t max_characters) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Generates a loopback-only self-signed certificate when the pair is missing. Existing files are
// never overwritten.
void ensure_world_certificate(const ServerLaunchOptions &options);

} // namespace chronicles::launcher
