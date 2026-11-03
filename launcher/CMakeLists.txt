# Launcher programs keep web/, resources/ and launcher-config.json beside the executable, so
# they share bin/<Config>/launcher/ instead of the common output folder.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/launcher")

add_library(chronicles-launcher-memory STATIC
  src/usermode_process_memory.cpp
)
target_include_directories(chronicles-launcher-memory PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(chronicles-launcher-memory PRIVATE
  _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

add_library(chronicles-launcher-core STATIC
  src/account_session.cpp
  src/discord_login.cpp
  src/discord_loopback.cpp
  src/agent_engine_redirect.cpp
  src/character_roster.cpp
  src/auth_redirect.cpp
  src/bootstrap_archive.cpp
  src/controller.cpp
  src/credential_store.cpp
  src/encoding.cpp
  src/fstring_patch.cpp
  src/game_process.cpp
  src/game_discovery.cpp
  src/game_compatibility.cpp
  src/release_servers.cpp
  src/http_client.cpp
  src/news_feed.cpp
  src/portal_feed.cpp
  src/patch_installer.cpp
  src/prerequisites.cpp
  src/playsharp_bridge.cpp
  src/server_process.cpp
  src/self_update.cpp
  src/settings.cpp
  src/unreal_string.cpp
  src/webview_host.cpp
  src/windows_command_line.cpp
  src/world_assist_process.cpp
  src/world_connection.cpp
)
target_include_directories(chronicles-launcher-core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/third_party/xorstr")
target_compile_definitions(chronicles-launcher-core PUBLIC JM_XORSTR_DISABLE_AVX_INTRINSICS)
target_link_libraries(chronicles-launcher-core PUBLIC
  chronicles-launcher-memory
  Boost::asio
  Boost::beast
  nlohmann_json::nlohmann_json
  unofficial::webview2::webview2
  chronicles-distribution
  chronicles-services-portal
)
target_link_libraries(chronicles-launcher-core PUBLIC
  advapi32
  bcrypt
  crypt32
  comctl32
  dwmapi
  ole32
  shell32
  shlwapi
  user32
  winhttp
  iphlpapi
  ws2_32
)
target_compile_definitions(chronicles-launcher-core PUBLIC
  _WIN32_WINNT=0x0A00
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  UNICODE
  _UNICODE
)
target_compile_definitions(chronicles-launcher-core PRIVATE
  CHRONICLES_LAUNCHER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

enable_language(RC)
add_executable(chronicles-launcher WIN32 src/main.cpp)
chronicles_process_resources(chronicles-launcher launcher "Project Chronicles Launcher")

add_executable(chronicles-launcher-operator WIN32 src/main.cpp)
target_include_directories(chronicles-launcher-operator PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources")
target_link_libraries(chronicles-launcher-operator PRIVATE chronicles-launcher-core)
target_compile_definitions(chronicles-launcher-operator PRIVATE
  CHRONICLES_LAUNCHER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
chronicles_process_resources(chronicles-launcher-operator launcher "Project Chronicles Operator Launcher")
# Both editions are named chronicles-launcher.exe and each needs its own launcher-config.json.
set_target_properties(chronicles-launcher-operator PROPERTIES OUTPUT_NAME "chronicles-launcher"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/launcher-operator")
target_include_directories(chronicles-launcher PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources")
target_link_libraries(chronicles-launcher PRIVATE chronicles-launcher-core)
if(MSVC)
  target_compile_options(chronicles-launcher PRIVATE $<$<CONFIG:Release>:/Zi>)
  target_link_options(chronicles-launcher PRIVATE $<$<CONFIG:Release>:/DEBUG>)
endif()
target_compile_definitions(chronicles-launcher PRIVATE
  CHRONICLES_LAUNCHER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  CHRONICLES_LAUNCHER_VERSION="${PROJECT_VERSION}"
)

# The executable resolves these assets beside itself, including in an installed package.
add_custom_target(chronicles-launcher-assets
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/web"
    "$<TARGET_FILE_DIR:chronicles-launcher>/web"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/profiles/player.json"
    "$<TARGET_FILE_DIR:chronicles-launcher>/launcher-config.json"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    "$<TARGET_FILE_DIR:chronicles-launcher>/resources"
  COMMENT "Copying Chronicles launcher interface"
)
add_dependencies(chronicles-launcher chronicles-launcher-assets)
add_custom_target(chronicles-operator-assets
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/web"
    "$<TARGET_FILE_DIR:chronicles-launcher-operator>/web"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/profiles/operator.json"
    "$<TARGET_FILE_DIR:chronicles-launcher-operator>/launcher-config.json"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    "$<TARGET_FILE_DIR:chronicles-launcher-operator>/resources"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:chronicles-world-tls-local>"
    "$<TARGET_FILE_DIR:chronicles-launcher-operator>/chronicles-world-tls-local.exe")
add_dependencies(chronicles-launcher-operator chronicles-operator-assets)

install(TARGETS chronicles-launcher
  RUNTIME DESTINATION . COMPONENT Launcher
)
install(DIRECTORY web DESTINATION . COMPONENT Launcher)
install(FILES "$<TARGET_FILE_DIR:chronicles-launcher>/WebView2Loader.dll"
  DESTINATION . COMPONENT Launcher
)
install(FILES $<TARGET_RUNTIME_DLLS:chronicles-launcher>
  DESTINATION . COMPONENT Launcher
)
install(DIRECTORY "$<TARGET_FILE_DIR:chronicles-launcher>/"
  DESTINATION . COMPONENT Launcher FILES_MATCHING PATTERN "*.dll" PATTERN "gtest*" EXCLUDE)
install(FILES DISTRIBUTION.md DESTINATION . COMPONENT Launcher)
install(FILES resources/legacy-game-runtime-files.json DESTINATION resources COMPONENT Launcher)
install(FILES
  "${PROJECT_SOURCE_DIR}/out/release-dependencies/MicrosoftEdgeWebview2Setup.exe"
  DESTINATION prerequisites COMPONENT Launcher)

add_executable(chronicles-world-tls-local src/world_tls_local.cpp)
target_link_libraries(chronicles-world-tls-local PRIVATE chronicles-launcher-core iphlpapi ws2_32)
chronicles_process_resources(chronicles-world-tls-local helper "Project Chronicles World Connection")
add_dependencies(chronicles-launcher chronicles-world-tls-local)
add_dependencies(chronicles-operator-assets chronicles-world-tls-local)
install(TARGETS chronicles-world-tls-local RUNTIME DESTINATION . COMPONENT Launcher)

add_executable(chronicles-zone-unlock
  src/zone_unlock_main.cpp
  src/zone_unlock.cpp
  src/encoding.cpp
)
target_include_directories(chronicles-zone-unlock PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(chronicles-zone-unlock PRIVATE
  nlohmann_json::nlohmann_json chronicles-launcher-memory)
target_compile_definitions(chronicles-zone-unlock PRIVATE
  NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

chronicles_process_resources(chronicles-zone-unlock helper "Project Chronicles Zone Helper")
add_dependencies(chronicles-launcher chronicles-zone-unlock)
install(TARGETS chronicles-zone-unlock RUNTIME DESTINATION . COMPONENT Launcher)

if(BUILD_TESTING)
  add_executable(chronicles-self-update-test-process src/self_update_test_process.cpp)
  target_link_libraries(chronicles-self-update-test-process PRIVATE chronicles-launcher-core)

  add_executable(chronicles-world-tls-tests src/world_tls_identity_tests.cpp)
  target_link_libraries(chronicles-world-tls-tests PRIVATE chronicles-launcher-core GTest::gtest_main)

  add_executable(chronicles-launcher-tests
    src/discord_login_tests.cpp
    src/account_session_tests.cpp
    src/agent_engine_redirect_tests.cpp
    src/auth_redirect_tests.cpp
    src/controller_tests.cpp
    src/game_process_tests.cpp
    src/world_connection_tests.cpp
    src/release_setup_tests.cpp
    src/self_update_tests.cpp
    src/news_feed_tests.cpp
    src/portal_feed_tests.cpp
    src/saved_login_tests.cpp
    src/patch_installer_tests.cpp
    src/zone_unlock_tests.cpp
    src/usermode_process_memory_tests.cpp
    src/zone_unlock.cpp
  )
  target_link_libraries(chronicles-launcher-tests PRIVATE
    chronicles-launcher-core
    GTest::gtest_main
  )
  include(GoogleTest)
  gtest_discover_tests(chronicles-world-tls-tests)
  # Saved-login and credential tests share the current user's Windows Credential Manager,
  # so parallel CTest processes must not run launcher test cases at the same time.
  gtest_discover_tests(chronicles-launcher-tests
    PROPERTIES RESOURCE_LOCK windows_credential_manager)
endif()
