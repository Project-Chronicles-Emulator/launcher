#include "world_assist_process.hpp"

#include "encoding.hpp"
#include "windows_command_line.hpp"

#include <windows.h>

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace chronicles::launcher {

struct WorldAssistProcess::Impl {
  PROCESS_INFORMATION process{};
  std::filesystem::path log_path;
  std::uint32_t last_exit_code{};
};

WorldAssistProcess::WorldAssistProcess() : impl_(std::make_unique<Impl>()) {}

WorldAssistProcess::~WorldAssistProcess() {
  if (impl_->process.hProcess != nullptr) {
    CloseHandle(impl_->process.hThread);
    CloseHandle(impl_->process.hProcess);
  }
}

void WorldAssistProcess::start(const std::filesystem::path &executable,
                               const std::filesystem::path &project_root,
                               const std::filesystem::path &log_path, std::uint32_t client_pid,
                               const std::filesystem::path &client_directory,
                               const std::string &backend_mode, const std::string &world_host,
                               int world_port) {
  if (is_running()) {
    throw std::runtime_error("The world entry helper is already running");
  }
  if (client_pid == 0 || client_directory.empty()) {
    throw std::invalid_argument("World entry requires a selected client and directory");
  }
  if (backend_mode != "playsharp" && backend_mode != "usermode") {
    throw std::invalid_argument("World entry requires a supported memory backend");
  }
  if (world_host.empty() != (world_port == 0) || world_port < 0 || world_port > 65535) {
    throw std::invalid_argument("World entry requires both a selected world host and port");
  }
  if (!std::filesystem::is_regular_file(executable)) {
    throw std::runtime_error("Build chronicles-world-tls-local before enabling world entry");
  }

  std::filesystem::create_directories(log_path.parent_path());
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  HANDLE log_file = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log_file == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("Could not create the world entry helper log");
  }

  LARGE_INTEGER log_offset{};
  if (!SetFilePointerEx(log_file, log_offset, nullptr, FILE_END)) {
    CloseHandle(log_file);
    throw std::runtime_error("Could not append to the world entry helper log");
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = log_file;
  startup.hStdError = log_file;

  std::vector<std::wstring> arguments{
      executable.wstring(),  L"--enter-world",           L"--pid",     std::to_wstring(client_pid),
      L"--client-directory", client_directory.wstring(), L"--backend", widen(backend_mode)};
  if (!world_host.empty()) {
    arguments.push_back(L"--world-host");
    arguments.push_back(widen(world_host));
    arguments.push_back(L"--world-port");
    arguments.push_back(std::to_wstring(world_port));
  }
  auto command = join_windows_command_line(arguments);
  const auto working_directory = project_root.wstring();
  const BOOL created =
      CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                     nullptr, working_directory.c_str(), &startup, &impl_->process);
  const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
  CloseHandle(log_file);
  if (!created) {
    throw std::runtime_error("Could not start the world entry helper (Windows error " +
                             std::to_string(create_error) + ")");
  }

  impl_->log_path = log_path;
  impl_->last_exit_code = 0;
}

bool WorldAssistProcess::is_running() {
  if (impl_->process.hProcess == nullptr) {
    return false;
  }
  const DWORD wait_result = WaitForSingleObject(impl_->process.hProcess, 0);
  if (wait_result == WAIT_TIMEOUT) {
    return true;
  }
  if (wait_result != WAIT_OBJECT_0) {
    throw std::runtime_error("Could not query the world entry helper process");
  }

  DWORD exit_code = 0;
  if (GetExitCodeProcess(impl_->process.hProcess, &exit_code)) {
    impl_->last_exit_code = exit_code;
  } else {
    impl_->last_exit_code = static_cast<std::uint32_t>(-1);
  }

  CloseHandle(impl_->process.hThread);
  CloseHandle(impl_->process.hProcess);
  impl_->process = PROCESS_INFORMATION{};
  return false;
}

std::uint32_t WorldAssistProcess::pid() const {
  return impl_->process.dwProcessId;
}

std::uint32_t WorldAssistProcess::last_exit_code() const {
  return impl_->last_exit_code;
}

std::string WorldAssistProcess::log_tail(std::size_t max_characters) const {
  std::ifstream input(impl_->log_path, std::ios::binary);
  if (!input) {
    return {};
  }

  std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (contents.size() > max_characters) {
    contents.erase(0, contents.size() - max_characters);
  }
  return contents;
}

} // namespace chronicles::launcher
