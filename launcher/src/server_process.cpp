#include "server_process.hpp"

#include "encoding.hpp"
#include "windows_command_line.hpp"

#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace chronicles::launcher {
namespace {

HANDLE open_log_file(const std::filesystem::path &path) {
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("Could not open log file: " + path.string());
  }
  return file;
}

std::vector<wchar_t>
build_environment_block(const std::map<std::wstring, std::wstring> &overrides) {
  struct CaseInsensitiveLess {
    bool operator()(const std::wstring &left, const std::wstring &right) const {
      return std::lexicographical_compare(
          left.begin(), left.end(), right.begin(), right.end(),
          [](wchar_t a, wchar_t b) { return std::towlower(a) < std::towlower(b); });
    }
  };

  std::map<std::wstring, std::wstring, CaseInsensitiveLess> environment;
  LPWCH raw = GetEnvironmentStringsW();
  if (raw == nullptr) {
    throw std::runtime_error("Could not read the launcher environment");
  }

  for (LPWCH entry = raw; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
    const std::wstring text(entry);
    const auto separator = text.find(L'=');
    if (separator != std::wstring::npos && separator > 0) {
      environment[text.substr(0, separator)] = text.substr(separator + 1);
    }
  }
  FreeEnvironmentStringsW(raw);

  for (const auto &[name, value] : overrides) {
    environment[name] = value;
  }

  std::vector<wchar_t> block;
  for (const auto &[name, value] : environment) {
    block.insert(block.end(), name.begin(), name.end());
    block.push_back(L'=');
    block.insert(block.end(), value.begin(), value.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

std::string read_file_tail(const std::filesystem::path &path, std::size_t max_characters) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {};
  }

  std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (contents.size() > max_characters) {
    contents.erase(0, contents.size() - max_characters);
  }
  return contents;
}

} // namespace

struct ServerProcess::Impl {
  PROCESS_INFORMATION process{};
  std::filesystem::path log_path;
  std::filesystem::path error_log_path;
};

ServerProcess::ServerProcess() : impl_(std::make_unique<Impl>()) {}

ServerProcess::~ServerProcess() {
  if (impl_->process.hProcess != nullptr) {
    CloseHandle(impl_->process.hProcess);
    CloseHandle(impl_->process.hThread);
  }
}

void ServerProcess::start(const ServerLaunchOptions &options) {
  if (is_running()) {
    throw std::runtime_error("The emulator server is already running");
  }
  if (!std::filesystem::is_regular_file(options.server_executable)) {
    throw std::runtime_error(
        "chronicles-server.exe was not found. Build the Release preset first.");
  }

  std::filesystem::create_directories(options.run_directory);
  if (options.world_enabled) {
    ensure_world_certificate(options);
  }

  std::vector<std::wstring> arguments{
      options.server_executable.wstring(), L"--storage",       L"memory", L"--port",
      std::to_wstring(options.http_port),  L"--native-client", L"local",  L"--protocol-dir",
      options.protocol_directory.wstring()};
  if (!options.client_data_directory.empty()) {
    arguments.insert(arguments.end(),
                     {L"--client-data-dir", options.client_data_directory.wstring()});
  }
  if (options.agent_engine_port != 0) {
    arguments.insert(arguments.end(), {L"--agent-engine-port",
                                       std::to_wstring(options.agent_engine_port)});
  }
  if (options.world_enabled) {
    arguments.insert(arguments.end(),
                     {L"--world-port", std::to_wstring(options.world_port), L"--tls-cert",
                      (options.run_directory / "localhost.pem").wstring(), L"--tls-key",
                      (options.run_directory / "localhost.key").wstring()});
    if (options.world_entry_diagnostic) {
      arguments.insert(arguments.end(),
                       {L"--world-handshake-delay-ms", L"2000",
                        L"--world-bootstrap-diagnostic", L"true"});
    }
  }

  const auto password_bytes = [] {
    std::vector<std::uint8_t> bytes(32);
    const auto status = BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
      throw std::runtime_error("Could not generate the disposable account password");
    }
    return bytes;
  }();

  std::map<std::wstring, std::wstring> environment{
      {L"CHRONICLES_BOOTSTRAP_ACCOUNT", widen(options.account)},
      {L"CHRONICLES_BOOTSTRAP_PASSWORD", widen(base64_encode(password_bytes))},
      {L"CHRONICLES_CLIENT_REGION", widen(options.region)},
  };

  impl_->log_path = options.run_directory / "server.log";
  impl_->error_log_path = options.run_directory / "server.error.log";
  HANDLE log_file = open_log_file(impl_->log_path);
  HANDLE error_log_file = open_log_file(impl_->error_log_path);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = log_file;
  startup.hStdError = error_log_file;

  auto command_wide = join_windows_command_line(arguments);
  auto environment_block = build_environment_block(environment);
  const auto working_directory = options.project_root.wstring();
  const BOOL created =
      CreateProcessW(options.server_executable.c_str(), command_wide.data(), nullptr, nullptr, TRUE,
                     CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment_block.data(),
                     working_directory.c_str(), &startup, &impl_->process);
  CloseHandle(log_file);
  CloseHandle(error_log_file);
  if (!created) {
    throw std::runtime_error("Could not start chronicles-server (Windows error " +
                             std::to_string(GetLastError()) + ")");
  }
}

void ServerProcess::stop() {
  if (impl_->process.hProcess == nullptr) {
    return;
  }

  TerminateProcess(impl_->process.hProcess, 0);
  WaitForSingleObject(impl_->process.hProcess, 10000);
  CloseHandle(impl_->process.hProcess);
  CloseHandle(impl_->process.hThread);
  impl_->process = PROCESS_INFORMATION{};
}

bool ServerProcess::is_running() {
  if (impl_->process.hProcess == nullptr) {
    return false;
  }
  if (WaitForSingleObject(impl_->process.hProcess, 0) == WAIT_TIMEOUT) {
    return true;
  }

  CloseHandle(impl_->process.hProcess);
  CloseHandle(impl_->process.hThread);
  impl_->process = PROCESS_INFORMATION{};
  return false;
}

std::uint32_t ServerProcess::pid() const {
  return impl_->process.dwProcessId;
}

std::string ServerProcess::log_tail(std::size_t max_characters) const {
  std::string logs = read_file_tail(impl_->log_path, max_characters);
  const auto errors = read_file_tail(impl_->error_log_path, max_characters);
  if (!errors.empty()) {
    logs += "\n--- stderr ---\n" + errors;
  }
  return logs;
}

void ensure_world_certificate(const ServerLaunchOptions &options) {
  const auto certificate = options.run_directory / "localhost.pem";
  const auto key = options.run_directory / "localhost.key";
  if (std::filesystem::is_regular_file(certificate) && std::filesystem::is_regular_file(key)) {
    return;
  }
  if (!std::filesystem::is_regular_file(options.openssl_executable)) {
    throw std::runtime_error("OpenSSL was not found. Set its path in Settings, or disable the "
                             "prototype world listener.");
  }

  const auto log_path = options.run_directory / "certificate.log";
  HANDLE log_file = open_log_file(log_path);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = log_file;
  startup.hStdError = log_file;

  const std::vector<std::wstring> arguments{options.openssl_executable.wstring(),
                                            L"req",
                                            L"-x509",
                                            L"-newkey",
                                            L"rsa:2048",
                                            L"-nodes",
                                            L"-days",
                                            L"30",
                                            L"-subj",
                                            L"/CN=localhost",
                                            L"-addext",
                                            L"subjectAltName=DNS:localhost,IP:127.0.0.1",
                                            L"-keyout",
                                            key.wstring(),
                                            L"-out",
                                            certificate.wstring()};

  auto command = join_windows_command_line(arguments);
  PROCESS_INFORMATION process{};
  const auto working_directory = options.run_directory.wstring();
  const BOOL created =
      CreateProcessW(options.openssl_executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                     CREATE_NO_WINDOW, nullptr, working_directory.c_str(), &startup, &process);
  CloseHandle(log_file);
  if (!created) {
    throw std::runtime_error("Could not start OpenSSL to create the local TLS certificate");
  }

  const auto wait = WaitForSingleObject(process.hProcess, 60000);
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  if (wait != WAIT_OBJECT_0 || exit_code != 0) {
    throw std::runtime_error("Local TLS certificate generation failed; see " + log_path.string());
  }
}

} // namespace chronicles::launcher
