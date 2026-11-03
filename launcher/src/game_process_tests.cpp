#include "playsharp_bridge.hpp"

#include <windows.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace chronicles::launcher {
namespace {

TEST(GameProcess, ExitedProcessDoesNotCountAsRunning) {
  EXPECT_NE(query_process_create_time(GetCurrentProcessId()), 0);

  wchar_t system_directory[MAX_PATH]{};
  const UINT directory_length = GetSystemDirectoryW(system_directory, MAX_PATH);
  ASSERT_GT(directory_length, 0u);
  ASSERT_LT(directory_length, MAX_PATH);

  const auto executable = std::filesystem::path(system_directory) / L"cmd.exe";
  auto command = L"\"" + executable.wstring() + L"\" /d /c exit 0";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  ASSERT_TRUE(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));

  const DWORD wait_result = WaitForSingleObject(process.hProcess, 10000);
  const auto exited_create_time = query_process_create_time(process.dwProcessId);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);

  ASSERT_EQ(WAIT_OBJECT_0, wait_result);
  EXPECT_EQ(exited_create_time, 0);
}

} // namespace
} // namespace chronicles::launcher
