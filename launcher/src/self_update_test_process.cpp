#include "encoding.hpp"
#include "release_servers.hpp"
#include "self_update.hpp"

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace chronicles::launcher;

int wmain(int argc, wchar_t **argv) {
  const auto *origin = _wgetenv(L"CHRONICLES_TEST_UPDATE_ORIGIN");
  const auto *key = _wgetenv(L"CHRONICLES_TEST_UPDATE_PUBLIC_KEY");
  const auto *root = _wgetenv(L"CHRONICLES_TEST_UPDATE_ROOT");
  if (origin == nullptr || key == nullptr || root == nullptr) {
    return 2;
  }

  const ReleaseServer server{"test", "Disposable test", narrow(origin), narrow(key),
                             "launcher-stable", true};
  try {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--test-parent") {
      return start_launcher_update_for_server(root, "0.2.5", server) ? 0 : 3;
    }
    if (argc == 5 && std::wstring_view(argv[1]) == L"--self-update-worker") {
      return run_launcher_update_worker_for_server(argv[2], std::stoull(argv[3]),
                                                    std::stoull(argv[4]), server);
    }
    if (argc == 1) {
      std::ofstream completed(std::filesystem::path(root) / "test-restarted.txt");
      completed << "updated process started";
      return completed ? 0 : 4;
    }
  } catch (const std::exception &error) {
    std::ofstream failure(std::filesystem::path(root) / "test-error.txt");
    failure << error.what();
    return 5;
  }
  return 6;
}
