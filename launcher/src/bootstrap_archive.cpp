#include "bootstrap_archive.hpp"
#include "encoding.hpp"
#include "http_client.hpp"
#include "patch_installer.hpp"
#include "windows_command_line.hpp"

#include <windows.h>

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace chronicles::launcher {
namespace {

void run_generator(const std::vector<std::wstring> &arguments,
                   const std::filesystem::path &working_directory,
                   const std::filesystem::path &log_path) {
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  HANDLE log_file = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log_file == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("Could not create the bootstrap generator log");
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = log_file;
  startup.hStdError = log_file;

  PROCESS_INFORMATION process{};
  auto command = join_windows_command_line(arguments);
  const auto working_path = working_directory.wstring();
  const BOOL created =
      CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                     working_path.c_str(), &startup, &process);
  const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
  CloseHandle(log_file);
  if (!created) {
    throw std::runtime_error("Could not start Python for local bootstrap generation (Windows "
                             "error " +
                             std::to_string(create_error) + ")");
  }

  const DWORD wait_result = WaitForSingleObject(process.hProcess, 120000);
  if (wait_result == WAIT_TIMEOUT) {
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, INFINITE);
  }
  DWORD exit_code = 1;
  if (wait_result == WAIT_OBJECT_0) {
    GetExitCodeProcess(process.hProcess, &exit_code);
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (wait_result != WAIT_OBJECT_0 || exit_code != 0) {
    throw std::runtime_error("Local bootstrap generation failed; inspect " + log_path.string());
  }
}

} // namespace

std::filesystem::path prepare_bootstrap_archive(const std::filesystem::path &project_root,
                                                const std::filesystem::path &run_directory,
                                                const std::filesystem::path &certificate,
                                                std::string_view bootstrap_url,
                                                bool redact_bootstrap_url) {
  const auto source_root = project_root / "out" / "native-config";
  const auto binary_config = source_root / "raw" / "X5" / "Config" / "BinaryConfig.ini";
  const auto dynamic_ini = source_root / "raw" / "X5" / "Config" / "DefaultX5Game.ini";
  const auto ini_names = source_root / "ini-bootstrap-names.candidate.json";
  const auto generator = project_root / "tools" / "binary_config_codec.py";
  if (!std::filesystem::is_regular_file(binary_config) ||
      !std::filesystem::is_regular_file(dynamic_ini) ||
      !std::filesystem::is_regular_file(ini_names) ||
      !std::filesystem::is_regular_file(generator) ||
      !std::filesystem::is_regular_file(certificate)) {
    throw std::runtime_error(
        "Local bootstrap inputs are missing under out/native-config or the server certificate");
  }

  std::filesystem::create_directories(run_directory);
  const auto output =
      run_directory / ("IniBootstrap.local-" + std::to_string(GetCurrentProcessId()) + "-" +
                       std::to_string(GetTickCount64()) + ".bin");
  const auto log_path = run_directory / "bootstrap-generator.log";
  auto certificate_value = certificate.generic_wstring();
  std::vector<std::wstring> arguments{L"python.exe",
                                      generator.wstring(),
                                      L"--input",
                                      binary_config.wstring(),
                                      L"--output",
                                      output.wstring(),
                                      L"--section",
                                      L"SSL",
                                      L"--key",
                                      L"DebuggingCertificatePath",
                                      L"--value",
                                      certificate_value,
                                      L"--engine-override",
                                      L"LwsWebSocket",
                                      L"bDisableCertValidation",
                                      L"False",
                                      L"--dynamic-ini",
                                      dynamic_ini.wstring(),
                                      L"--dynamic-filename",
                                      L"../../../X5/Config/DefaultX5Game.ini",
                                      L"--global-ini-names",
                                      ini_names.wstring(),
                                      L"--bootstrap-url",
                                      widen(std::string(bootstrap_url))};
  if (redact_bootstrap_url) {
    arguments.push_back(L"--redact-bootstrap-url");
  }

  run_generator(arguments, project_root, log_path);
  if (!std::filesystem::is_regular_file(output)) {
    throw std::runtime_error("Bootstrap generator exited without producing an archive");
  }
  return output;
}

std::filesystem::path prepare_signed_bootstrap(const distribution::Release &release,
                                               const std::filesystem::path &run_directory,
                                               const std::string &bootstrap_url,
                                               std::stop_token stop) {
  if (!release.bootstrap) {
    throw std::runtime_error("Signed release is missing its bootstrap template");
  }
  std::filesystem::create_directories(run_directory);
  const auto &entry = *release.bootstrap;
  const auto template_path = run_directory / ("bootstrap-template-" + entry.sha256 + ".bin");
  require_regular_patch_path(template_path);
  if (!std::filesystem::is_regular_file(template_path) ||
      distribution::sha256_file(template_path) != entry.sha256) {
    http_download_file(release.api_origin + std::string(distribution::kBlobRoute) + entry.sha256,
                       template_path, entry.size, {}, stop);
  }
  auto bytes = distribution::read_bounded_file(template_path, distribution::kMaximumBootstrapBytes);
  if (bytes.size() != entry.size || distribution::sha256_bytes(bytes) != entry.sha256) {
    throw std::runtime_error("Client bootstrap template failed SHA-256 verification");
  }
  const auto placeholder = release.api_origin + "/launch/" + std::string(64, '0') + "/";
  const auto offset = bytes.find(placeholder);
  if (bootstrap_url.size() != placeholder.size() || offset == std::string::npos ||
      bytes.find(placeholder, offset + 1) != std::string::npos || offset < 4 ||
      offset + placeholder.size() >= bytes.size() || bytes[offset + placeholder.size()] != '\0') {
    throw std::runtime_error("Signed bootstrap template has an invalid grant placeholder");
  }
  std::uint32_t stored_length{};
  for (unsigned index = 0; index < 4; ++index) {
    stored_length |=
        static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset - 4 + index]))
        << (index * 8);
  }
  if (stored_length != placeholder.size() + 1) {
    throw std::runtime_error(
        "Bootstrap placeholder is not an ASCII FString of the expected length");
  }
  bytes.replace(offset, placeholder.size(), bootstrap_url);
  const auto output_path =
      run_directory / ("IniBootstrap.remote-" + std::to_string(GetCurrentProcessId()) + "-" +
                       std::to_string(GetTickCount64()) + ".bin");
  require_regular_patch_path(output_path);
  std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.flush();
  SecureZeroMemory(bytes.data(), bytes.size());
  if (!output) {
    throw std::runtime_error("Cannot write the account-specific client bootstrap archive");
  }
  return output_path;
}

} // namespace chronicles::launcher
