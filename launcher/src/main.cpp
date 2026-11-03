#include "controller.hpp"
#include "encoding.hpp"
#include "prerequisites.hpp"
#include "resource.h"
#include "self_update.hpp"
#include "patch_installer.hpp"
#include "webview_host.hpp"

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace chronicles::launcher {
namespace {

using json = nlohmann::json;

#ifdef CHRONICLES_PLAYER_RELEASE
constexpr bool kLiveServersOnly = true;
#else
constexpr bool kLiveServersOnly = false;
#endif

struct UiAction {
  std::string name;
  json payload;
};

class ActionQueue {
public:
  void push(UiAction action) {
    {
      std::scoped_lock lock(mutex_);
      actions_.push_back(std::move(action));
    }
    condition_.notify_all();
  }

  std::optional<UiAction> pop() {
    std::scoped_lock lock(mutex_);
    if (actions_.empty()) {
      return std::nullopt;
    }

    auto action = std::move(actions_.front());
    actions_.pop_front();
    return action;
  }

  void wake() {
    condition_.notify_all();
  }

  void wait_for(std::chrono::milliseconds duration, const std::atomic<bool> &finished) {
    std::unique_lock lock(mutex_);
    condition_.wait_for(lock, duration, [&] { return finished.load() || !actions_.empty(); });
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<UiAction> actions_;
};

void open_web_link(const std::string &url) {
  const auto wide_url = widen(url);
  // URL handlers can use STA shell extensions; this action runs on the worker thread.
  const HRESULT apartment =
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  if (FAILED(apartment)) {
    throw std::runtime_error("Windows could not initialize the browser link handler.");
  }
  struct ApartmentCleanup {
    ~ApartmentCleanup() {
      CoUninitialize();
    }
  } cleanup;

  const auto result = reinterpret_cast<INT_PTR>(
      ShellExecuteW(nullptr, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) {
    throw std::runtime_error("Windows could not open this link in your browser.");
  }
}

void dispatch_action(LauncherController &controller, UiAction &action) {
  if (action.name == "ui_ready") {
    controller.log("Player interface connected");
    controller.publish_state();
  } else if (action.name == "start_server") {
    controller.start_server();
  } else if (action.name == "stop_server") {
    controller.stop_server();
  } else if (action.name == "launch_game") {
    const auto account = action.payload.value("account", std::string{});
    if (!action.payload.contains("password") || !action.payload.at("password").is_string()) {
      controller.launch_game({}, account);
    } else {
      auto &password = action.payload.at("password").get_ref<std::string &>();
      controller.launch_game(std::move(password), account);
    }
  } else if (action.name == "sign_in") {
    auto &password = action.payload.at("password").get_ref<std::string &>();
    controller.sign_in(std::move(password), action.payload.at("account").get<std::string>(),
                       action.payload.value("keep_signed_in", false),
                       action.payload.value("remember_password", false),
                       action.payload.value("use_saved_password", false));
  } else if (action.name == "sign_out") {
    controller.sign_out();
  } else if (action.name == "sign_in_discord") {
    const auto url = controller.begin_discord_login(action.payload.value("keep_signed_in", false));
    try {
      open_web_link(url);
    } catch (...) {
      controller.cancel_discord_login();
      throw;
    }
  } else if (action.name == "cancel_discord_login") {
    controller.cancel_discord_login();
  } else if (action.name == "save_login_preferences") {
    controller.set_login_preferences(action.payload.at("keep_signed_in").get<bool>(),
                                     action.payload.at("remember_password").get<bool>());
  } else if (action.name == "forget_saved_login") {
    controller.forget_saved_login();
  } else if (action.name == "patch_now") {
    controller.apply_redirect();
  } else if (action.name == "restore_now") {
    controller.restore_redirect();
  } else if (action.name == "select_server") {
    controller.select_server(action.payload.at("id").get<std::string>());
  } else if (action.name == "browse_game_directory") {
    controller.browse_game_directory();
  } else if (action.name == "save_settings") {
    controller.save_player_settings(action.payload);
  } else if (action.name == "set_auto_redirect") {
    controller.set_auto_redirect(action.payload.value("enabled", false));
  } else if (action.name == "refresh") {
    controller.refresh();
  } else if (action.name == "refresh_news") {
    controller.refresh_news();
  } else if (action.name == "refresh_content") {
    controller.refresh_content();
  } else if (action.name == "refresh_characters") {
    controller.refresh_characters();
  } else if (action.name == "open_portal_link") {
    open_web_link(controller.portal_link(action.payload.at("kind").get<std::string>(),
                                         action.payload.at("id").get<std::string>()));
  } else if (action.name == "check_updates") {
    controller.check_updates();
  } else if (action.name == "update_game") {
    controller.update_game();
  } else if (action.name == "verify_game") {
    controller.update_game(true);
  } else {
    controller.note_error("Unknown launcher action: " + action.name);
  }
}

int run_cli(LauncherController &controller, const std::string &command) {
  controller.set_state_sink([](const std::string &) {});
  if (command == "--status" || command == "--prepare-bootstrap" || command == "--check-updates" ||
      command == "--update") {
    controller.probe();
  } else {
    controller.initialize();
  }

  if (command == "--patch") {
    controller.apply_redirect();
  } else if (command == "--restore") {
    controller.restore_from_latest_backup();
  } else if (command == "--status") {
    // probe() already ran; refreshing here would start an automatic client scan.
  } else if (command == "--prepare-bootstrap") {
    std::cout << controller.prepare_local_bootstrap().string() << "\n";
    return 0;
  } else if (command == "--check-updates") {
    controller.check_updates();
  } else if (command == "--update") {
    controller.update_game(true);
  } else {
    std::cerr << "Unknown command\n";
    return 2;
  }

  std::cout << controller.state().dump(2) << "\n";
  std::cout.flush();
  return 0;
}

int run_window(std::filesystem::path project_root, std::filesystem::path launcher_directory,
               const std::filesystem::path &ui_directory) {
  const auto data_directory = project_root / "out" / "launcher" / "webview2";

  LauncherController controller(project_root, launcher_directory, kLiveServersOnly);
  ActionQueue actions;
  WebViewHost host;
  WebViewWindowOptions window_options;
  window_options.icon_resource_id = IDI_CHRONICLES_LAUNCHER;

  host.create(
      ui_directory, data_directory,
      [&](const json &message) {
        if (message.value("action", std::string{}) == "cancel_update") {
          controller.cancel_update();
          return;
        }
        actions.push(UiAction{message.value("action", std::string{}),
                              message.value("payload", json::object())});
      },
      window_options);
  controller.set_state_sink([&](const std::string &state) { host.post_state(state); });

  std::atomic<bool> finished{false};
  std::thread worker([&] {
    try {
      controller.initialize();
    } catch (const std::exception &error) {
      controller.note_error(error.what());
    }

    while (!finished.load()) {
      while (auto action = actions.pop()) {
        try {
          if (action->name == "register_account") {
            auto &password = action->payload.at("password").get_ref<std::string &>();
            const auto account = controller.register_account(
                std::move(password), action->payload.at("account").get<std::string>());
            host.post_event(
                json{{"type", "registration_result"}, {"success", true}, {"account", account}}
                    .dump());
          } else {
            dispatch_action(controller, *action);
          }
          if (action->name == "save_settings") {
            host.post_event(json{{"type", "settings_result"}, {"success", true}}.dump());
          }
          if (action->name == "sign_in" || action->name == "sign_out" ||
              action->name == "save_login_preferences" || action->name == "forget_saved_login") {
            host.post_event(json{{"type", "auth_result"}, {"success", true}}.dump());
          }
        } catch (const std::exception &error) {
          controller.note_error(error.what());
          if (action->name == "register_account") {
            host.post_event(
                json{{"type", "registration_result"}, {"success", false}, {"error", error.what()}}
                    .dump());
          }
          if (action->name == "save_settings") {
            host.post_event(
                json{{"type", "settings_result"}, {"success", false}, {"error", error.what()}}
                    .dump());
          }
          if (action->name == "sign_in" || action->name == "sign_out" ||
              action->name == "save_login_preferences" || action->name == "forget_saved_login") {
            host.post_event(
                json{{"type", "auth_result"}, {"success", false}, {"error", error.what()}}.dump());
          }
        }
      }

      try {
        controller.refresh();
      } catch (const std::exception &error) {
        controller.note_error(error.what());
      }

      actions.wait_for(std::chrono::milliseconds(1500), finished);
    }
  });

  const int result = host.run_message_loop();

  finished.store(true);
  controller.request_stop();
  actions.wake();
  worker.join();
  controller.shutdown();
  return result;
}

} // namespace
} // namespace chronicles::launcher

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  using namespace chronicles::launcher;

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  int argc = 0;
  const auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) {
    return 1;
  }

  const auto launcher_directory = std::filesystem::path(CHRONICLES_LAUNCHER_SOURCE_DIR);
  std::vector<wchar_t> module_path(32768);
  const auto path_length =
      GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
  if (path_length == 0 || path_length >= module_path.size()) {
    LocalFree(argv);
    return 1;
  }
  const auto executable_directory = std::filesystem::path(module_path.data()).parent_path();

#ifdef CHRONICLES_PLAYER_RELEASE
  if (argc == 5 && std::wstring_view(argv[1]) == L"--self-update-worker") {
    try {
      const auto inherited_handle = std::stoull(argv[3]);
      const auto ready_handle = std::stoull(argv[4]);
      const auto result = run_launcher_update_worker(argv[2], inherited_handle, ready_handle);
      LocalFree(argv);
      return result;
    } catch (const std::exception &error) {
      MessageBoxA(nullptr, error.what(), "Project Chronicles Launcher Update", MB_OK | MB_ICONERROR);
      LocalFree(argv);
      return 1;
    }
  }
#endif

  const auto source_root = launcher_directory.parent_path();
  // Both launcher editions build below bin/ (launcher/ and launcher-operator/).
  const auto development_directory = source_root / "out" / "build" / "windows" / "bin";
  const auto relative_executable = executable_directory.lexically_relative(development_directory);
  const bool development_build =
      !relative_executable.empty() && *relative_executable.begin() != L"..";
  auto project_root = development_build ? source_root : executable_directory / "runtime";
  std::string command;
  bool skip_self_update = false;
  for (int index = 1; index < argc; ++index) {
    if (std::wstring_view(argv[index]) == L"--skip-self-update-once") {
      skip_self_update = true;
    } else if (std::wstring_view(argv[index]) == L"--project-root" && index + 1 < argc) {
      project_root = std::filesystem::absolute(argv[++index]);
    } else if (command.empty() && std::wstring_view(argv[index]) != L"--project-root") {
      command = narrow(argv[index]);
    } else {
      MessageBoxW(nullptr, L"Invalid launcher arguments. Use --help for usage.", L"Project Chronicles Launcher",
                  MB_OK | MB_ICONERROR);
      LocalFree(argv);
      return 2;
    }
  }
  const bool cli_mode = !command.empty();

#ifdef CHRONICLES_PLAYER_RELEASE
  if (!development_build && !cli_mode) {
    const bool recovering = std::filesystem::exists(
        launcher_update_journal_path(executable_directory));
    if (!skip_self_update || recovering) {
      try {
        if (start_launcher_update(executable_directory, CHRONICLES_LAUNCHER_VERSION)) {
          LocalFree(argv);
          return 0;
        }
      } catch (const std::exception &error) {
        // A failed optional update check must not block Play with a dialog; the check runs
        // again at the next start. Only an interrupted installation needs the player's attention.
        if (recovering) {
          MessageBoxA(nullptr, error.what(), "Project Chronicles Launcher Update", MB_OK | MB_ICONWARNING);
          LocalFree(argv);
          return 1;
        }
      }
    }
  }
#endif

  if (command == "--help") {
    MessageBoxA(nullptr,
                "chronicles-launcher [--status|--patch|--restore|--prepare-bootstrap|"
                "--check-updates|--update]\n"
                "[--project-root PATH]\n"
                "Without a command, opens the WebView2 launcher window.\n"
                "Installed builds use the adjacent runtime folder unless --project-root is set.",
                "Project Chronicles Launcher", MB_OK | MB_ICONINFORMATION);
    LocalFree(argv);
    return 0;
  }

  // GUI-subsystem applications still inherit redirected standard handles, so the CLI commands
  // write their JSON through normal stdout without attaching to a console.

  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) {
    LocalFree(argv);
    return 1;
  }

  int result = 1;
  try {
    if (command == "--setup-runtime") {
      ensure_webview_runtime(executable_directory, false);
      std::cout << "{\"runtime_ready\":true}\n";
      result = 0;
    } else if (cli_mode) {
      LauncherController controller(project_root, executable_directory, kLiveServersOnly);
      result = run_cli(controller, command);
    } else {
      ensure_webview_runtime(executable_directory);
      result = run_window(project_root, executable_directory, executable_directory / "web");
    }
  } catch (const std::exception &error) {
    if (cli_mode) {
      std::cerr << "Error: " << error.what() << "\n";
    } else {
      MessageBoxA(nullptr, error.what(), "Project Chronicles Launcher", MB_OK | MB_ICONERROR);
    }
  }

  CoUninitialize();
  LocalFree(argv);
  return result;
}
