#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace chronicles::launcher {

// Runs the guarded, one-connection world entry helper independently of the UI worker. The helper
// restores its temporary client changes before exiting.
class WorldAssistProcess {
public:
  WorldAssistProcess();
  ~WorldAssistProcess();

  WorldAssistProcess(const WorldAssistProcess &) = delete;
  WorldAssistProcess &operator=(const WorldAssistProcess &) = delete;

  void start(const std::filesystem::path &executable, const std::filesystem::path &project_root,
             const std::filesystem::path &log_path, std::uint32_t client_pid,
             const std::filesystem::path &client_directory, const std::string &backend_mode,
             const std::string &world_host = {}, int world_port = 0);

  bool is_running();

  std::uint32_t pid() const;

  std::uint32_t last_exit_code() const;

  std::string log_tail(std::size_t max_characters) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace chronicles::launcher
