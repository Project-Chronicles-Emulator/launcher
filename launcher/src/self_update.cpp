#include "self_update.hpp"

#include "encoding.hpp"
#include "game_compatibility.hpp"
#include "http_client.hpp"
#include "patch_installer.hpp"
#include "release_servers.hpp"
#include "windows_command_line.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace chronicles::launcher {
namespace {

constexpr std::string_view kChannel = "launcher-stable";
constexpr std::uint64_t kMaximumLauncherBytes = 256ULL * 1024 * 1024;
constexpr std::size_t kMaximumLauncherFiles = 256;
constexpr wchar_t kStageDirectory[] = L".chronicles-launcher-update";

struct HandleCloser {
  void operator()(void *handle) const {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};

using Handle = std::unique_ptr<void, HandleCloser>;

std::array<unsigned int, 3> parse_version(std::string_view version) {
  std::array<unsigned int, 3> components{};
  for (std::size_t index = 0; index < components.size(); ++index) {
    const auto separator = version.find('.');
    const auto component = version.substr(0, separator);
    if (component.empty() || (component.size() > 1 && component.front() == '0')) {
      throw std::invalid_argument("Launcher version must contain three decimal components");
    }
    const auto result = std::from_chars(component.data(), component.data() + component.size(),
                                        components[index]);
    if (result.ec != std::errc{} || result.ptr != component.data() + component.size()) {
      throw std::invalid_argument("Invalid launcher version component");
    }
    if (index + 1 == components.size()) {
      if (separator != std::string_view::npos) {
        throw std::invalid_argument("Launcher version has too many components");
      }
    } else {
      if (separator == std::string_view::npos) {
        throw std::invalid_argument("Launcher version has too few components");
      }
      version.remove_prefix(separator + 1);
    }
  }
  return components;
}

bool allowed_launcher_file(const std::string &path) {
  if (!distribution::valid_patch_path(path)) {
    return false;
  }

  if (path == "chronicles-launcher.exe" || path == "chronicles-world-tls-local.exe" ||
      path == "chronicles-zone-unlock.exe" || path == "launcher-config.json" ||
      path == "README.txt" || path == "prerequisites/MicrosoftEdgeWebview2Setup.exe" ||
      path == "resources/legacy-game-runtime-files.json") {
    return true;
  }
  if (path.starts_with("web/") || path.starts_with("licenses/")) {
    return true;
  }
  return path.find('/') == std::string::npos && path.ends_with(".dll");
}

ReleaseServer player_server(const std::filesystem::path &directory) {
  const auto servers = release_servers(directory);
  const auto selected = std::ranges::find(servers, std::string("live-na"), &ReleaseServer::id);
  if (selected == servers.end() || selected->local) {
    throw std::runtime_error("The player launcher update server is unavailable");
  }
  return *selected;
}

distribution::Release verified_release(const std::string &envelope,
                                       const ReleaseServer &server) {
  auto release = distribution::verify_release(envelope, server.patch_public_key,
                                               std::string(kChannel), server.origin);
  validate_launcher_release(release);
  return release;
}

void verify_staged_files(const std::filesystem::path &stage,
                         const distribution::Release &release) {
  for (const auto &file : release.files) {
    const auto path = stage / file.path;
    require_regular_patch_path(path);
    if (!std::filesystem::is_regular_file(path) ||
        std::filesystem::file_size(path) != file.size ||
        distribution::sha256_file(path) != file.sha256) {
      throw std::runtime_error("A staged launcher file failed verification: " + file.path);
    }
  }
  const auto profile = nlohmann::json::parse(
      distribution::read_bounded_file(stage / "launcher-config.json", 65536));
  if (profile.value("edition", std::string{}) != "player") {
    throw std::runtime_error("Launcher update does not contain a player profile");
  }
  // The game preparation path depends on this exact ownership receipt.
  legacy_game_runtime_files(stage);
}

Handle launch_process(const std::filesystem::path &executable,
                      const std::vector<std::wstring> &arguments,
                      bool inherit_handles = false) {
  auto command = join_windows_command_line(arguments);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, inherit_handles, 0, nullptr,
                      executable.parent_path().c_str(), &startup, &process)) {
    throw std::runtime_error("Could not start the launcher update process (Windows error " +
                             std::to_string(GetLastError()) + ")");
  }
  CloseHandle(process.hThread);
  return Handle(process.hProcess);
}

void start_worker(const std::filesystem::path &directory,
                  const std::filesystem::path &stage) {
  const auto executable = stage / L"chronicles-launcher.exe";
  Handle parent(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE,
                            GetCurrentProcessId()));
  if (!parent) {
    throw std::runtime_error("Could not create a launcher update synchronization handle");
  }
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  Handle ready(CreateEventW(&attributes, TRUE, FALSE, nullptr));
  if (!ready) {
    throw std::runtime_error("Could not create the launcher update readiness event");
  }

  auto worker = launch_process(
      executable,
      {executable.wstring(), L"--self-update-worker", directory.wstring(),
       std::to_wstring(reinterpret_cast<std::uintptr_t>(parent.get())),
       std::to_wstring(reinterpret_cast<std::uintptr_t>(ready.get()))}, true);
  if (WaitForSingleObject(ready.get(), 30000) != WAIT_OBJECT_0) {
    TerminateProcess(worker.get(), 1);
    throw std::runtime_error("The launcher update worker did not become ready");
  }
}

} // namespace

int compare_launcher_versions(std::string_view left, std::string_view right) {
  const auto left_components = parse_version(left);
  const auto right_components = parse_version(right);
  if (left_components < right_components) {
    return -1;
  }
  if (left_components > right_components) {
    return 1;
  }
  return 0;
}

void validate_launcher_release(const distribution::Release &release) {
  parse_version(release.version);
  if (release.channel != kChannel || release.bootstrap || release.files.empty() ||
      release.files.size() > kMaximumLauncherFiles || release.total_bytes > kMaximumLauncherBytes) {
    throw std::invalid_argument("Invalid launcher update manifest");
  }
  constexpr std::array<std::string_view, 15> required_paths{
      "chronicles-launcher.exe", "launcher-config.json", "WebView2Loader.dll",
      "libcrypto-3-x64.dll", "chronicles-world-tls-local.exe", "chronicles-zone-unlock.exe",
      "resources/legacy-game-runtime-files.json", "prerequisites/MicrosoftEdgeWebview2Setup.exe",
      "web/index.html", "web/app.js", "web/app.css", "web/portal.js",
      "web/assets/fonts/InterVariable.woff2", "web/assets/fonts/Cinzel-Variable.ttf",
      "web/assets/world-harbor.png"};
  std::array<bool, required_paths.size()> required{};
  for (const auto &file : release.files) {
    if (!allowed_launcher_file(file.path)) {
      throw std::invalid_argument("Launcher update contains a protected path: " + file.path);
    }
    for (std::size_t index = 0; index < required_paths.size(); ++index) {
      required[index] = required[index] || file.path == required_paths[index];
    }
  }
  if (std::ranges::find(required, false) != required.end()) {
    throw std::invalid_argument("Launcher update is missing a required player file");
  }
}

bool start_launcher_update(const std::filesystem::path &launcher_directory,
                           const std::string &current_version) {
  return start_launcher_update_for_server(launcher_directory, current_version,
                                          player_server(launcher_directory));
}

bool start_launcher_update_for_server(const std::filesystem::path &launcher_directory,
                                      const std::string &current_version,
                                      const ReleaseServer &server) {
  const auto directory = std::filesystem::absolute(launcher_directory).lexically_normal();
  const auto update_root = directory / kStageDirectory;
  const auto envelope_path = update_root / L"manifest.json";
  const bool recovering =
      std::filesystem::exists(launcher_update_journal_path(directory));

  std::string envelope;
  if (recovering) {
    envelope = distribution::read_bounded_file(envelope_path, distribution::kMaximumManifestBytes);
  } else {
    try {
      envelope = http_get_text(server.origin + "/distribution/v1/channels/" +
                                   std::string(kChannel),
                               3000);
    } catch (const HttpError &error) {
      if (error.status() == 404) {
        return false;
      }
      throw;
    }
  }

  const auto release = verified_release(envelope, server);
  // The installer already stores the full digest in its transaction and marker. A shorter
  // directory name keeps nested Windows staging paths within ordinary path-length limits.
  const auto stage = update_root / L"payload" / release.payload_sha256.substr(0, 16);
  if (!recovering && compare_launcher_versions(release.version, current_version) <= 0) {
    return false;
  }
  if (!recovering) {
    PatchInstaller installer(stage);
    installer.install(
        release,
        [&](const distribution::PatchFile &file, const std::filesystem::path &destination,
            const std::function<void(std::uint64_t)> &progress, std::stop_token stop) {
          http_download_file(server.origin + std::string(distribution::kBlobRoute) + file.sha256,
                             destination, file.size, progress, stop);
        },
        [](std::uint64_t, std::uint64_t, const std::string &, const std::string &) {}, {}, [] {});
    std::ofstream output(envelope_path, std::ios::binary | std::ios::trunc);
    output << envelope;
    output.flush();
    if (!output) {
      throw std::runtime_error("Could not save the signed launcher update manifest");
    }
  }
  verify_staged_files(stage, release);
  start_worker(directory, stage);
  return true;
}

int run_launcher_update_worker(const std::filesystem::path &launcher_directory,
                               std::uintptr_t inherited_parent_handle,
                               std::uintptr_t inherited_ready_handle) {
  return run_launcher_update_worker_for_server(launcher_directory, inherited_parent_handle,
                                                inherited_ready_handle,
                                                player_server(launcher_directory));
}

int run_launcher_update_worker_for_server(const std::filesystem::path &launcher_directory,
                                          std::uintptr_t inherited_parent_handle,
                                          std::uintptr_t inherited_ready_handle,
                                          const ReleaseServer &server) {
  const auto directory = std::filesystem::absolute(launcher_directory).lexically_normal();
  const auto envelope = distribution::read_bounded_file(
      directory / kStageDirectory / L"manifest.json", distribution::kMaximumManifestBytes);
  const auto release = verified_release(envelope, server);
  const auto stage = directory / kStageDirectory / L"payload" /
                     release.payload_sha256.substr(0, 16);
  std::array<wchar_t, 32768> own_path{};
  const auto own_length = GetModuleFileNameW(nullptr, own_path.data(),
                                             static_cast<DWORD>(own_path.size()));
  if (own_length == 0 || own_length >= own_path.size() ||
      _wcsicmp(own_path.data(), (stage / L"chronicles-launcher.exe").c_str()) != 0) {
    throw std::runtime_error("Launcher update worker is outside its verified staging directory");
  }
  verify_staged_files(stage, release);

  Handle parent(reinterpret_cast<HANDLE>(inherited_parent_handle));
  if (!parent) {
    throw std::runtime_error("Could not identify the launcher being updated");
  }
  std::array<wchar_t, 32768> process_path{};
  DWORD process_path_size = static_cast<DWORD>(process_path.size());
  if (!QueryFullProcessImageNameW(parent.get(), 0, process_path.data(), &process_path_size) ||
      _wcsicmp(process_path.data(), (directory / L"chronicles-launcher.exe").c_str()) != 0) {
    throw std::runtime_error("Launcher update parent does not match the installation");
  }
  Handle ready(reinterpret_cast<HANDLE>(inherited_ready_handle));
  if (!ready || !SetEvent(ready.get())) {
    throw std::runtime_error("Could not signal launcher update readiness");
  }
  if (WaitForSingleObject(parent.get(), 60000) != WAIT_OBJECT_0) {
    throw std::runtime_error("The old launcher did not close for its update");
  }

  const auto executable = directory / L"chronicles-launcher.exe";
  try {
    PatchInstaller installer(directory, ExistingDirectory::launcher);
    installer.recover_pending();
    installer.install(
        release,
        [&](const distribution::PatchFile &file, const std::filesystem::path &destination,
            const std::function<void(std::uint64_t)> &progress, std::stop_token) {
          std::filesystem::copy_file(stage / file.path, destination,
                                     std::filesystem::copy_options::overwrite_existing);
          progress(file.size);
        },
        [](std::uint64_t, std::uint64_t, const std::string &, const std::string &) {}, {}, [] {});
  } catch (const std::exception &error) {
    MessageBoxA(nullptr, error.what(), "Project Chronicles Launcher Update", MB_OK | MB_ICONWARNING);
    if (!std::filesystem::exists(launcher_update_journal_path(directory))) {
      launch_process(executable, {executable.wstring(), L"--skip-self-update-once"});
    }
    return 1;
  }

  launch_process(executable, {executable.wstring()});
  return 0;
}

} // namespace chronicles::launcher
