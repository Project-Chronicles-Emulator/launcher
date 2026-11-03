#include "distribution/distribution.hpp"
#include "encoding.hpp"
#include "game_process.hpp"
#include "playsharp_bridge.hpp"
#include "settings.hpp"
#include "usermode_process_memory.hpp"
#include "world_connection.hpp"
#include "world_tls_identity.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace chronicles::launcher {
namespace {

// These addresses and bytes belong only to build 40367e24241c9481bfc07fbf9ef1919f25fb0eaa.
// The code signatures are checked before any client memory is written.
constexpr std::uint64_t kEngineGlobal = 0x14C380590;
constexpr std::uint64_t kVerifierCode = 0x141FE6610;
constexpr std::uint64_t kWorldVerifyCallback = 0x1461E8860;
constexpr std::uint64_t kAlwaysAcceptCallback = 0x14186A540;
constexpr std::uint64_t kStartupGate = 0x146170ED1;
constexpr std::uint64_t kConnectionVtable = 0x14A81F598;
constexpr std::uint64_t kEngineContextsOffset = 0x11C0;
constexpr std::uint64_t kEngineContextCountOffset = 0x11C8;
constexpr std::uint64_t kContextGameInstanceOffset = 0x208;
constexpr std::uint64_t kGameInstanceWorldNetOffset = 0x260;
constexpr std::uint64_t kWorldNetConnectionOffset = 0x5880;
constexpr std::uint64_t kConnectionOwnerOffset = 0x08;
constexpr std::uint64_t kConnectionSocketOffset = 0x10;
constexpr std::uint64_t kConnectionSendThreadOffset = 0x70;
constexpr std::uint64_t kConnectionReceiveThreadOffset = 0x78;
constexpr std::uint64_t kConnectionActiveOffset = 0x90;
constexpr std::uint64_t kConnectionSendThreadStateOffset = 0x94;
constexpr std::uint64_t kConnectionStopRequestedOffset = 0x9C;
constexpr std::uint64_t kConnectionTlsEnabledOffset = 0xC8;
constexpr std::uint64_t kConnectionTlsWrapperOffset = 0xD0;
constexpr std::uint64_t kWrapperSslOffset = 0x20;
constexpr std::uint64_t kSslVerifyModeOffset = 0x558;
constexpr std::uint64_t kSslVerifyCallbackOffset = 0x560;
constexpr std::uint64_t kSslContextOffset = 0x598;
constexpr std::uint64_t kSslVerifyResultOffset = 0x5A8;
constexpr std::uint64_t kSslContextAppCallbackOffset = 0x98;
constexpr std::uint64_t kMaximumContexts = 64;
constexpr std::string_view kHostedClientExecutableSha256 =
    "e8c01d7c0e180efa2f7b60687ba5625b2779a314809dab255299c72cbe97c547";
constexpr std::array<std::uint8_t, 16> kVerifierBytes = {
    0x40, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0xC0, 0xF5, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x40, 0x0B, 0x00};
constexpr std::array<std::uint8_t, 16> kCallbackBytes = {
    0xE8, 0x10, 0xA1, 0xD1, 0x06, 0x78, 0x68, 0x66, 0xC8, 0x5E, 0x89, 0xD0, 0x17, 0x7A, 0x7C, 0x88};
constexpr std::array<std::uint8_t, 6> kAlwaysAcceptCallbackBytes = {0xB8, 0x01, 0x00,
                                                                    0x00, 0x00, 0xC3};
constexpr std::array<std::uint8_t, 2> kStartupGateOriginal = {0x84, 0xC0};
constexpr std::array<std::uint8_t, 2> kStartupGateOverride = {0x0C, 0x01};

constexpr auto kPollInterval = std::chrono::milliseconds(40);
constexpr auto kStatePollInterval = std::chrono::milliseconds(1);
constexpr auto kRestoreDelay = std::chrono::seconds(5);
constexpr auto kClientInitializationWait = std::chrono::minutes(3);
constexpr auto kWorldConnectionWait = std::chrono::minutes(10);

std::atomic_bool interrupted{false};

BOOL WINAPI handle_console_control(DWORD event) {
  if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
    interrupted.store(true);
    return TRUE;
  }
  return FALSE;
}

struct WorldSsl {
  std::uint64_t connection{};
  std::uint64_t wrapper{};
  std::uint64_t ssl{};
};

struct ConnectionState {
  bool retired{};
  bool socket_present{};
  bool wrapper_present{};
  bool send_thread_present{};
  bool receive_thread_present{};
  std::uint64_t active{};
  std::uint64_t send_thread_state{};
  std::uint64_t stop_requested{};
  std::optional<std::uint64_t> verify_result;

  bool operator==(const ConnectionState &) const = default;
};

enum class Trial {
  OpenSslDefault,
  VerifyNone,
  AlwaysAcceptCallback,
  EnterWorld,
};

bool should_stop() {
  return interrupted.load();
}

std::uint64_t read_unsigned(ProcessMemory &memory, std::uint64_t address, std::size_t width) {
  const auto bytes = memory.read(address, width);
  if (bytes.size() != width) {
    throw MemoryAccessError("Short client memory read");
  }

  std::uint64_t value = 0;
  for (std::size_t index = 0; index < width; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
  }
  return value;
}

bool client_image_matches(ProcessMemory &memory) {
  return memory.read(kVerifierCode, kVerifierBytes.size()) ==
             std::vector<std::uint8_t>(kVerifierBytes.begin(), kVerifierBytes.end()) &&
         memory.read(kWorldVerifyCallback, kCallbackBytes.size()) ==
             std::vector<std::uint8_t>(kCallbackBytes.begin(), kCallbackBytes.end());
}

void check_client_image(ProcessMemory &memory) {
  if (!client_image_matches(memory)) {
    throw std::runtime_error("Client code does not match the researched build");
  }
}

void check_always_accept_callback(ProcessMemory &memory) {
  const auto expected_bytes = std::vector<std::uint8_t>(kAlwaysAcceptCallbackBytes.begin(),
                                                        kAlwaysAcceptCallbackBytes.end());
  if (memory.read(kAlwaysAcceptCallback, expected_bytes.size()) != expected_bytes) {
    throw std::runtime_error("Existing success callback does not match the researched build");
  }
}

using ProtectStartupGate = std::function<std::uint32_t(std::uint32_t)>;
using VerifyProcessIdentity = std::function<void()>;

void write_startup_gate(ProcessMemory &memory, const ProtectStartupGate &protect,
                        const VerifyProcessIdentity &verify_identity, bool enable_override) {
  const auto &expected = enable_override ? kStartupGateOriginal : kStartupGateOverride;
  const auto &replacement = enable_override ? kStartupGateOverride : kStartupGateOriginal;
  const std::vector<std::uint8_t> expected_bytes(expected.begin(), expected.end());
  const std::vector<std::uint8_t> replacement_bytes(replacement.begin(), replacement.end());

  verify_identity();
  if (memory.read(kStartupGate, expected_bytes.size()) != expected_bytes) {
    throw std::runtime_error("World startup gate does not contain the expected client bytes");
  }

  const auto original_protection = protect(PAGE_EXECUTE_READWRITE);

  try {
    memory.write(kStartupGate, replacement_bytes);
    if (memory.read(kStartupGate, replacement_bytes.size()) != replacement_bytes) {
      throw std::runtime_error("World startup gate write did not pass readback");
    }
  } catch (...) {
    try {
      if (memory.read(kStartupGate, replacement_bytes.size()) == replacement_bytes) {
        memory.write(kStartupGate, expected_bytes);
      }
      protect(original_protection);
    } catch (const std::exception &) {
      std::cerr << "Could not confirm startup gate restoration after a failed write.\n";
    }
    throw;
  }

  try {
    protect(original_protection);
  } catch (...) {
    if (enable_override) {
      try {
        memory.write(kStartupGate, expected_bytes);
        protect(original_protection);
      } catch (const std::exception &) {
        std::cerr << "Could not confirm startup gate restoration after protection failed.\n";
      }
    }
    throw;
  }
}

class StartupGateGuard {
public:
  StartupGateGuard(ProcessMemory &memory, ProtectStartupGate protect,
                   VerifyProcessIdentity verify_identity)
      : memory_(memory), protect_(std::move(protect)),
        verify_identity_(std::move(verify_identity)) {}

  ~StartupGateGuard() {
    try {
      restore();
    } catch (const std::exception &error) {
      std::cerr << "Startup gate restore failed: " << error.what() << '\n';
    }
  }

  void arm() {
    write_startup_gate(memory_, protect_, verify_identity_, true);
    armed_ = true;
    std::cout << "Temporarily enabled the protected world startup branch.\n" << std::flush;
  }

  void restore() {
    if (!armed_) {
      return;
    }

    const std::vector<std::uint8_t> override_bytes(kStartupGateOverride.begin(),
                                                   kStartupGateOverride.end());
    if (memory_.read(kStartupGate, override_bytes.size()) != override_bytes) {
      throw std::runtime_error("Startup gate changed; original bytes were not written over it");
    }

    write_startup_gate(memory_, protect_, verify_identity_, false);
    armed_ = false;
    std::cout << "Restored and verified the original startup gate bytes.\n" << std::flush;
  }

  bool armed() const {
    return armed_;
  }

private:
  ProcessMemory &memory_;
  ProtectStartupGate protect_;
  VerifyProcessIdentity verify_identity_;
  bool armed_{};
};

std::optional<WorldSsl> find_world_ssl(ProcessMemory &memory) {
  const auto engine = read_unsigned(memory, kEngineGlobal, 8);
  if (engine == 0) {
    return std::nullopt;
  }

  const auto contexts = read_unsigned(memory, engine + kEngineContextsOffset, 8);
  const auto context_count = read_unsigned(memory, engine + kEngineContextCountOffset, 4);
  if (contexts == 0 || context_count == 0 || context_count > kMaximumContexts) {
    return std::nullopt;
  }

  for (std::uint64_t index = 0; index < context_count; ++index) {
    const auto context = read_unsigned(memory, contexts + index * 8, 8);
    if (context == 0 || read_unsigned(memory, context, 1) != 1) {
      continue;
    }

    const auto game_instance = read_unsigned(memory, context + kContextGameInstanceOffset, 8);
    if (game_instance == 0) {
      continue;
    }
    const auto world_net = read_unsigned(memory, game_instance + kGameInstanceWorldNetOffset, 8);
    if (world_net == 0) {
      continue;
    }
    const auto connection = read_unsigned(memory, world_net + kWorldNetConnectionOffset, 8);
    if (connection == 0 || read_unsigned(memory, connection, 8) != kConnectionVtable ||
        read_unsigned(memory, connection + kConnectionOwnerOffset, 8) != world_net ||
        read_unsigned(memory, connection + kConnectionTlsEnabledOffset, 1) != 1) {
      continue;
    }

    const auto wrapper = read_unsigned(memory, connection + kConnectionTlsWrapperOffset, 8);
    if (wrapper == 0) {
      continue;
    }
    const auto ssl = read_unsigned(memory, wrapper + kWrapperSslOffset, 8);
    if (ssl == 0) {
      continue;
    }
    const auto callback = read_unsigned(memory, ssl + kSslVerifyCallbackOffset, 8);
    if (callback != kWorldVerifyCallback && callback != 0 && callback != kAlwaysAcceptCallback) {
      continue;
    }

    const auto ssl_context = read_unsigned(memory, ssl + kSslContextOffset, 8);
    if (ssl_context == 0 ||
        read_unsigned(memory, ssl_context + kSslContextAppCallbackOffset, 8) != 0) {
      continue;
    }
    return WorldSsl{connection, wrapper, ssl};
  }
  return std::nullopt;
}

bool is_same_world_ssl(ProcessMemory &memory, const WorldSsl &target) {
  const auto current = find_world_ssl(memory);
  return current && current->connection == target.connection &&
         current->wrapper == target.wrapper && current->ssl == target.ssl;
}

ConnectionState read_connection_state(ProcessMemory &memory, const WorldSsl &target) {
  ConnectionState state;

  if (read_unsigned(memory, target.connection, 8) != kConnectionVtable) {
    state.retired = true;
    return state;
  }

  state.socket_present = read_unsigned(memory, target.connection + kConnectionSocketOffset, 8) != 0;
  state.send_thread_present =
      read_unsigned(memory, target.connection + kConnectionSendThreadOffset, 8) != 0;
  state.receive_thread_present =
      read_unsigned(memory, target.connection + kConnectionReceiveThreadOffset, 8) != 0;
  state.active = read_unsigned(memory, target.connection + kConnectionActiveOffset, 1);
  state.send_thread_state =
      read_unsigned(memory, target.connection + kConnectionSendThreadStateOffset, 4);
  state.stop_requested =
      read_unsigned(memory, target.connection + kConnectionStopRequestedOffset, 1);

  const auto wrapper = read_unsigned(memory, target.connection + kConnectionTlsWrapperOffset, 8);
  state.wrapper_present = wrapper != 0;
  if (wrapper == target.wrapper &&
      read_unsigned(memory, wrapper + kWrapperSslOffset, 8) == target.ssl) {
    state.verify_result = read_unsigned(memory, target.ssl + kSslVerifyResultOffset, 8);
  }

  return state;
}

void print_connection_state(const ConnectionState &state) {
  if (state.retired) {
    std::cout << "World connection object retired.\n" << std::flush;
    return;
  }

  std::cout << "World state: socket=" << state.socket_present
            << " wrapper=" << state.wrapper_present << " active=" << state.active
            << " send_thread=" << state.send_thread_present
            << " receive_thread=" << state.receive_thread_present
            << " send_thread_state=" << state.send_thread_state
            << " stop_requested=" << state.stop_requested;
  if (state.verify_result) {
    std::cout << " verify_result=" << *state.verify_result;
  }
  std::cout << '\n' << std::flush;
}

void write_ssl_field(ProcessMemory &memory, const WorldSsl &target, std::uint64_t offset,
                     std::size_t width, std::uint64_t expected, std::uint64_t replacement) {
  if (!is_same_world_ssl(memory, target) ||
      read_unsigned(memory, target.ssl + offset, width) != expected) {
    throw std::runtime_error("World TLS object changed before the diagnostic write");
  }

  std::array<std::uint8_t, 8> bytes{};
  std::memcpy(bytes.data(), &replacement, width);
  memory.write(target.ssl + offset, std::span(bytes.data(), width));
  if (read_unsigned(memory, target.ssl + offset, width) != replacement) {
    throw std::runtime_error("World TLS diagnostic readback failed");
  }
}

class SslOverrideGuard {
public:
  SslOverrideGuard(ProcessMemory &memory, Trial trial) : memory_(memory), trial_(trial) {}

  ~SslOverrideGuard() {
    try {
      restore();
    } catch (const std::exception &error) {
      std::cerr << "TLS callback restore failed: " << error.what() << '\n';
    }
  }

  void apply(const WorldSsl &target) {
    target_ = target;
    try {
      write_ssl_field(memory_, target, offset(), width(), original(), replacement());
    } catch (...) {
      try {
        restore();
      } catch (const std::exception &error) {
        std::cerr << "TLS callback restore after write failure: " << error.what() << '\n';
      }
      throw;
    }

    if (trial_ == Trial::OpenSslDefault) {
      std::cout << "Using OpenSSL's default verifier for this world TLS object.\n";
    } else if (trial_ == Trial::VerifyNone) {
      std::cout << "Temporarily set SSL_VERIFY_NONE on this world TLS object.\n";
    } else {
      std::cout << "Using an existing success callback for this world TLS object.\n";
    }
    std::cout << std::flush;
  }

  void restore() {
    if (!target_) {
      return;
    }

    try {
      if (is_same_world_ssl(memory_, *target_) &&
          read_unsigned(memory_, target_->ssl + offset(), width()) == replacement()) {
        write_ssl_field(memory_, *target_, offset(), width(), replacement(), original());
        std::cout << "Restored the original SSL field on the same TLS object.\n";
      } else {
        std::cout << "The TLS object ended before restore; no live override remains.\n";
      }
    } catch (const MemoryAccessError &) {
      std::cout << "The TLS object ended before restore; no live override remains.\n";
    }
    target_.reset();
  }

private:
  std::uint64_t offset() const {
    return trial_ == Trial::VerifyNone ? kSslVerifyModeOffset : kSslVerifyCallbackOffset;
  }

  std::size_t width() const {
    return trial_ == Trial::VerifyNone ? 4 : 8;
  }

  std::uint64_t original() const {
    return trial_ == Trial::VerifyNone ? 1 : kWorldVerifyCallback;
  }

  std::uint64_t replacement() const {
    if (trial_ == Trial::VerifyNone || trial_ == Trial::OpenSslDefault) {
      return 0;
    }
    return kAlwaysAcceptCallback;
  }

  ProcessMemory &memory_;
  Trial trial_;
  std::optional<WorldSsl> target_;
};

void run_local_override(const LauncherSettings &settings, Trial trial,
                        std::optional<std::uint32_t> client_pid,
                        const std::optional<std::string> &world_host,
                        const std::optional<int> &world_port) {
  if ((settings.backend_mode != "playsharp" && settings.backend_mode != "usermode") ||
      !settings.world_enabled) {
    throw std::runtime_error("This diagnostic requires a memory backend and world listener");
  }
  const bool hosted_world = world_host.has_value();
  if (hosted_world != world_port.has_value()) {
    throw std::invalid_argument("A hosted world requires both its host and port");
  }
  if (hosted_world && (trial != Trial::EnterWorld || settings.backend_mode != "usermode")) {
    throw std::invalid_argument("Hosted world entry requires the guarded user-mode path");
  }
  const WorldConnectionTarget connection_target(hosted_world ? *world_host : "127.0.0.1",
                                                hosted_world ? *world_port : settings.world_port);

  const auto client = client_pid ? find_client_process_by_pid(settings.game_directory, *client_pid)
                                 : find_client_process(settings.game_directory);
  if (!client || client->create_time == 0) {
    throw std::runtime_error("The requested game client is not running at the expected path");
  }
  if (hosted_world &&
      distribution::sha256_file(client->executable) != kHostedClientExecutableSha256) {
    throw std::runtime_error("Hosted world TLS requires the researched game executable");
  }

  std::unique_ptr<PlaySharpBridge> bridge;
  std::unique_ptr<ProcessMemory> memory_owner;
  ProtectStartupGate protect_startup_gate;
  VerifyProcessIdentity verify_identity;

  if (settings.backend_mode == "playsharp") {
    bridge = std::make_unique<PlaySharpBridge>();
    bridge->start(settings.bridge_executable);

    auto bridge_memory =
        std::make_unique<BridgeProcessMemory>(*bridge, client->pid, settings.backend_mode);
    auto *verified_memory = bridge_memory.get();
    verify_identity = [verified_memory]() { verified_memory->verify_process_identity(); };
    protect_startup_gate = [bridge = bridge.get(),
                            pid = client->pid](std::uint32_t new_protection) {
      const nlohmann::json request{{"pid", pid},
                                   {"address", format_address(kStartupGate)},
                                   {"size", kStartupGateOriginal.size()},
                                   {"protect", new_protection},
                                   {"mode", "playsharp"}};
      const auto result = bridge->call("virtual_protect", request, {});
      return result.at("old_protect").get<std::uint32_t>();
    };
    memory_owner = std::move(bridge_memory);
  } else {
    auto usermode_memory = std::make_unique<UsermodeProcessMemory>(client->pid, client->executable);
    auto *verified_memory = usermode_memory.get();
    verify_identity = [pid = client->pid, creation_time = client->create_time]() {
      if (query_process_create_time(pid) != creation_time) {
        throw MemoryAccessError("Selected client identity changed");
      }
    };
    protect_startup_gate = [verified_memory](std::uint32_t new_protection) {
      return verified_memory->protect(kStartupGate, kStartupGateOriginal.size(), new_protection);
    };
    memory_owner = std::move(usermode_memory);
  }

  ProcessMemory &memory = *memory_owner;

  const auto initialization_deadline = std::chrono::steady_clock::now() + kClientInitializationWait;
  std::cout << "Waiting for the researched client code to initialize.\n" << std::flush;
  while (std::chrono::steady_clock::now() < initialization_deadline && !should_stop()) {
    if (query_process_create_time(client->pid) != client->create_time) {
      throw std::runtime_error("The game client exited or changed identity");
    }

    try {
      if (client_image_matches(memory)) {
        break;
      }
    } catch (const MemoryAccessError &) {
      // The protected code can still be initializing during game startup.
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  check_client_image(memory);

  StartupGateGuard startup_gate(memory, std::move(protect_startup_gate),
                                std::move(verify_identity));
  SslOverrideGuard ssl_override(memory, hosted_world ? Trial::OpenSslDefault : trial);

  std::cout << "Watching client " << client->pid << " for world TLS at " << connection_target.host()
            << ':' << connection_target.port() << ". Select a character now.\n"
            << std::flush;
  const auto deadline = std::chrono::steady_clock::now() + kWorldConnectionWait;
  std::optional<WorldSsl> patched;
  while (std::chrono::steady_clock::now() < deadline && !should_stop()) {
    if (query_process_create_time(client->pid) != client->create_time) {
      throw std::runtime_error("The game client exited or changed identity");
    }

    if (connection_target.has_connection(client->pid)) {
      try {
        const auto current = find_world_ssl(memory);
        if (current &&
            read_unsigned(memory, current->ssl + kSslVerifyCallbackOffset, 8) ==
                kWorldVerifyCallback &&
            read_unsigned(memory, current->ssl + kSslVerifyModeOffset, 4) == 1 &&
            read_unsigned(memory, current->ssl + kSslVerifyResultOffset, 8) == 0 &&
            read_unsigned(memory, current->connection + kConnectionSendThreadOffset, 8) == 0) {
          check_client_image(memory);
          if (hosted_world) {
            require_hosted_tls_identity(memory, current->ssl, connection_target.host());
            std::cout << "Confirmed certificate hostname for the selected hosted world.\n";
          }
          if (trial == Trial::AlwaysAcceptCallback || trial == Trial::EnterWorld) {
            if (!hosted_world) {
              check_always_accept_callback(memory);
            }
            if (trial == Trial::EnterWorld) {
              const std::vector<std::uint8_t> original_gate(kStartupGateOriginal.begin(),
                                                            kStartupGateOriginal.end());
              if (memory.read(kStartupGate, original_gate.size()) != original_gate) {
                throw std::runtime_error("Startup gate does not match the researched build");
              }
              startup_gate.arm();
            }
          }
          ssl_override.apply(*current);
          patched = current;
          break;
        }
      } catch (const MemoryAccessError &) {
        if (startup_gate.armed()) {
          throw;
        }
        // The pending TLS object can disappear while a failed connection is retired.
      }
    }
    std::this_thread::sleep_for(kPollInterval);
  }

  if (!patched) {
    if (should_stop()) {
      throw std::runtime_error("Cancelled before changing client memory");
    }
    throw std::runtime_error("No matching selected world TLS object appeared within ten minutes");
  }

  const auto restore_at = std::chrono::steady_clock::now() + kRestoreDelay;
  std::optional<ConnectionState> previous_state;
  while (std::chrono::steady_clock::now() < restore_at && !should_stop()) {
    try {
      const auto state = read_connection_state(memory, *patched);
      if (state != previous_state) {
        print_connection_state(state);
        previous_state = state;
      }
      if (state.retired) {
        break;
      }
    } catch (const MemoryAccessError &) {
      std::cout << "World connection object became unreadable.\n" << std::flush;
      break;
    }
    std::this_thread::sleep_for(kStatePollInterval);
  }
  ssl_override.restore();
  startup_gate.restore();
  if (hosted_world && (!connection_target.has_connection(client->pid) || !previous_state ||
                       previous_state->retired || !previous_state->send_thread_present ||
                       !previous_state->verify_result || *previous_state->verify_result != 0)) {
    throw std::runtime_error("Hosted world TLS did not stay verified and connected");
  }
}

} // namespace
} // namespace chronicles::launcher

int wmain(int argc, wchar_t **argv) {
  try {
    auto trial = chronicles::launcher::Trial::OpenSslDefault;
    std::optional<std::uint32_t> client_pid;
    std::optional<std::filesystem::path> client_directory;
    std::optional<std::string> backend_override;
    std::optional<std::string> world_host;
    std::optional<int> world_port;
    bool trial_selected = false;

    for (int index = 1; index < argc; ++index) {
      const std::wstring_view argument(argv[index]);
      if (argument == L"--pid" && index + 1 < argc && !client_pid) {
        const std::wstring value(argv[++index]);
        std::size_t parsed = 0;
        const auto number = std::stoull(value, &parsed, 10);
        if (parsed != value.size() || number == 0 ||
            number > std::numeric_limits<std::uint32_t>::max()) {
          throw std::invalid_argument("Client PID must be a positive 32-bit integer");
        }
        client_pid = static_cast<std::uint32_t>(number);
      } else if (argument == L"--client-directory" && index + 1 < argc && !client_directory) {
        client_directory = std::filesystem::path(argv[++index]);
      } else if (argument == L"--backend" && index + 1 < argc && !backend_override) {
        const std::wstring_view value(argv[++index]);
        if (value == L"usermode") {
          backend_override = "usermode";
        } else if (value == L"playsharp") {
          backend_override = "playsharp";
        } else {
          throw std::invalid_argument("Backend must be usermode or playsharp");
        }
      } else if (argument == L"--world-host" && index + 1 < argc && !world_host) {
        world_host = chronicles::launcher::narrow(argv[++index]);
      } else if (argument == L"--world-port" && index + 1 < argc && !world_port) {
        const std::wstring value(argv[++index]);
        std::size_t parsed = 0;
        const auto number = std::stoull(value, &parsed, 10);
        if (parsed != value.size() || number == 0 || number > 65535) {
          throw std::invalid_argument("World port must be between 1 and 65535");
        }
        world_port = static_cast<int>(number);
      } else if (!trial_selected && argument == L"--verify-none") {
        trial = chronicles::launcher::Trial::VerifyNone;
        trial_selected = true;
      } else if (!trial_selected && argument == L"--always-accept-callback") {
        trial = chronicles::launcher::Trial::AlwaysAcceptCallback;
        trial_selected = true;
      } else if (!trial_selected && argument == L"--enter-world") {
        trial = chronicles::launcher::Trial::EnterWorld;
        trial_selected = true;
      } else {
        throw std::invalid_argument("Usage: chronicles-world-tls-local "
                                    "[--verify-none|--always-accept-callback|--enter-world] "
                                    "[--pid PROCESS_ID] [--client-directory PATH] "
                                    "[--backend usermode|playsharp] "
                                    "[--world-host DNS_NAME --world-port PORT]");
      }
    }

    if (!SetConsoleCtrlHandler(chronicles::launcher::handle_console_control, TRUE)) {
      throw std::runtime_error("Could not register the console restore handler");
    }

    const auto project_root = std::filesystem::current_path();
    chronicles::launcher::SettingsStore settings_store(project_root / "out" / "launcher");
    auto settings = settings_store.load();
    if (client_directory) {
      settings.game_directory = *client_directory;
    }
    if (backend_override) {
      settings.backend_mode = *backend_override;
    }

    chronicles::launcher::run_local_override(settings, trial, client_pid, world_host, world_port);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "World TLS local diagnostic: " << error.what() << '\n';
    return 1;
  }
}
