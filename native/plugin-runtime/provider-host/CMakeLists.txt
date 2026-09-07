find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)

add_library(omarchy-plugin-provider-host STATIC
  src/provider_catalog.cpp
  src/provider_host.cpp
)
target_include_directories(omarchy-plugin-provider-host PUBLIC include)
target_link_libraries(omarchy-plugin-provider-host PUBLIC
  omarchy-plugin-capability-definitions
  omarchy-plugin-launcher)

set(OMARCHY_COMMAND_CONTRACT_DIGEST
  "58dff89884b20c17e8b4bca08c95d0fdf25587625b9777cd900f710d34e375b4")
set(OMARCHY_DESKTOP_OPEN_CONTRACT_DIGEST
  "d678287160034f2311446423b8c1d6847b80abc0ea62233036c578611387622c")
set(OMARCHY_SYSTEM_OBSERVE_CONTRACT_DIGEST
  "e32bc94a49b0e2170465f7e7c7d671a6a4a7d1d857cee4b4111b81dd3088c977")
set(OMARCHY_NETWORK_FETCH_CONTRACT_DIGEST
  "24d172b971ac832c86d4e28c224bfd7e912aa1ea3de4517aa09af2c02d362a20")
set(OMARCHY_MEDIA_STREAM_CONTRACT_DIGEST
  "f060bfec526a4355bd3431d39310bf96e61907f26be0e0dea6e6e8c2d28a415c")
configure_file(cmake/command_contract.hpp.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/command_contract.hpp" @ONLY)
add_executable(omarchy-plugin-command-executor
  src/command_executor.cpp)
target_include_directories(omarchy-plugin-command-executor PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(omarchy-plugin-command-executor PRIVATE
  Qt6::Core)
install(TARGETS omarchy-plugin-command-executor
  RUNTIME DESTINATION "${OMARCHY_PLUGIN_INSTALL_ROOT}/bin")
add_executable(omarchy-plugin-desktop-opener
  src/desktop_open_provider.cpp)
target_compile_definitions(omarchy-plugin-desktop-opener PRIVATE
  OMARCHY_DESKTOP_OPEN_CONTRACT_DIGEST="${OMARCHY_DESKTOP_OPEN_CONTRACT_DIGEST}")
target_link_libraries(omarchy-plugin-desktop-opener PRIVATE Qt6::Core)
install(TARGETS omarchy-plugin-desktop-opener
  RUNTIME DESTINATION "${OMARCHY_PLUGIN_INSTALL_ROOT}/bin")
add_executable(omarchy-plugin-system-observer
  src/system_observe_provider.cpp)
target_compile_definitions(omarchy-plugin-system-observer PRIVATE
  OMARCHY_SYSTEM_OBSERVE_CONTRACT_DIGEST="${OMARCHY_SYSTEM_OBSERVE_CONTRACT_DIGEST}")
target_link_libraries(omarchy-plugin-system-observer PRIVATE Qt6::Core)
install(TARGETS omarchy-plugin-system-observer
  RUNTIME DESTINATION "${OMARCHY_PLUGIN_INSTALL_ROOT}/bin")
add_executable(omarchy-plugin-network-media-provider
  src/network_media_provider.cpp)
target_compile_definitions(omarchy-plugin-network-media-provider PRIVATE
  OMARCHY_NETWORK_FETCH_CONTRACT_DIGEST="${OMARCHY_NETWORK_FETCH_CONTRACT_DIGEST}"
  OMARCHY_MEDIA_STREAM_CONTRACT_DIGEST="${OMARCHY_MEDIA_STREAM_CONTRACT_DIGEST}")
target_link_libraries(omarchy-plugin-network-media-provider PRIVATE
  PkgConfig::CURL Qt6::Core)
install(TARGETS omarchy-plugin-network-media-provider
  RUNTIME DESTINATION "${OMARCHY_PLUGIN_INSTALL_ROOT}/bin")
configure_file(cmake/install_providers.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/install_providers.cmake" @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/install_providers.cmake")

if(BUILD_TESTING)
  omarchy_plugin_test(plugin-provider-frame COMMAND omarchy-plugin-provider-frame-test
    LABELS "unit;security;providers;protocol;adversarial"
    SOURCES tests/provider_frame_test.cpp)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(omarchy-plugin-network-media-provider-test-peer
    src/network_media_provider.cpp)
  target_compile_definitions(omarchy-plugin-network-media-provider-test-peer PRIVATE
    OMARCHY_NETWORK_MEDIA_TESTING=1
    OMARCHY_PROVIDER_CHANNEL_FD=0
    OMARCHY_NETWORK_FETCH_CONTRACT_DIGEST="${OMARCHY_NETWORK_FETCH_CONTRACT_DIGEST}"
    OMARCHY_MEDIA_STREAM_CONTRACT_DIGEST="${OMARCHY_MEDIA_STREAM_CONTRACT_DIGEST}")
  target_link_libraries(omarchy-plugin-network-media-provider-test-peer PRIVATE
    PkgConfig::CURL Qt6::Core)
  # End-to-end companion for the QEMU connection-time network policy lane
  # (T08): the real network_media_provider with live curl/getaddrinfo and no
  # OMARCHY_NETWORK_MEDIA_TESTING short-circuits, driven over stdin (fd 0) so the
  # protocol peer scripts can drive it directly. Build artifact only, not a test.
  add_executable(omarchy-plugin-network-media-provider-e2e-peer
    src/network_media_provider.cpp)
  target_compile_definitions(omarchy-plugin-network-media-provider-e2e-peer PRIVATE
    OMARCHY_PROVIDER_CHANNEL_FD=0
    OMARCHY_NETWORK_FETCH_CONTRACT_DIGEST="${OMARCHY_NETWORK_FETCH_CONTRACT_DIGEST}"
    OMARCHY_MEDIA_STREAM_CONTRACT_DIGEST="${OMARCHY_MEDIA_STREAM_CONTRACT_DIGEST}")
  target_link_libraries(omarchy-plugin-network-media-provider-e2e-peer PRIVATE
    PkgConfig::CURL Qt6::Core)
  omarchy_plugin_test(plugin-network-media-provider
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/network_media_provider_test.py
      $<TARGET_FILE:omarchy-plugin-network-media-provider-test-peer>
    TIMEOUT 10 LABELS "unit;security;providers;network;media;adversarial")

  add_executable(omarchy-plugin-command-executor-test-peer
    src/command_executor.cpp)
  target_compile_definitions(omarchy-plugin-command-executor-test-peer PRIVATE
    OMARCHY_COMMAND_EXECUTOR_TESTING=1)
  target_include_directories(omarchy-plugin-command-executor-test-peer PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_link_libraries(omarchy-plugin-command-executor-test-peer PRIVATE
    Qt6::Core)

  omarchy_plugin_test(plugin-command-executor
    COMMAND omarchy-plugin-command-executor-test
    TIMEOUT 10 LABELS "unit;security;providers;commands;adversarial"
    SOURCES tests/command_executor_test.cpp
    LIBRARIES Qt6::Core)
  target_compile_definitions(omarchy-plugin-command-executor-test PRIVATE
    COMMAND_EXECUTOR_PATH="$<TARGET_FILE:omarchy-plugin-command-executor-test-peer>"
    COMMAND_CONTRACT_DIGEST="${OMARCHY_COMMAND_CONTRACT_DIGEST}"
    COMMAND_SCOPE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/tests")
  add_dependencies(omarchy-plugin-command-executor-test
    omarchy-plugin-command-executor-test-peer)

  add_executable(omarchy-plugin-desktop-opener-test-peer
    src/desktop_open_provider.cpp)
  target_compile_definitions(omarchy-plugin-desktop-opener-test-peer PRIVATE
    OMARCHY_DESKTOP_OPEN_CONTRACT_DIGEST="${OMARCHY_DESKTOP_OPEN_CONTRACT_DIGEST}"
    OMARCHY_DESKTOP_OPEN_TESTING=1)
  target_link_libraries(omarchy-plugin-desktop-opener-test-peer PRIVATE Qt6::Core)

  omarchy_plugin_test(plugin-desktop-opener COMMAND omarchy-plugin-desktop-opener-test
    TIMEOUT 5 LABELS "unit;security;providers;desktop;gesture;adversarial"
    SOURCES tests/desktop_open_provider_test.cpp
    LIBRARIES Qt6::Core)
  target_compile_definitions(omarchy-plugin-desktop-opener-test PRIVATE
    DESKTOP_OPENER_PATH="$<TARGET_FILE:omarchy-plugin-desktop-opener-test-peer>"
    DESKTOP_OPEN_CONTRACT_DIGEST="${OMARCHY_DESKTOP_OPEN_CONTRACT_DIGEST}")
  add_dependencies(omarchy-plugin-desktop-opener-test
    omarchy-plugin-desktop-opener-test-peer)

  add_executable(omarchy-plugin-package-probe-test-peer
    tests/package_probe_peer.cpp)
  add_executable(omarchy-plugin-system-observer-test-peer
    src/system_observe_provider.cpp)
  target_compile_definitions(omarchy-plugin-system-observer-test-peer PRIVATE
    OMARCHY_SYSTEM_OBSERVE_CONTRACT_DIGEST="${OMARCHY_SYSTEM_OBSERVE_CONTRACT_DIGEST}"
    OMARCHY_SYSTEM_OBSERVE_CHECKUPDATES="$<TARGET_FILE:omarchy-plugin-package-probe-test-peer>"
    OMARCHY_SYSTEM_OBSERVE_PACMAN="$<TARGET_FILE:omarchy-plugin-package-probe-test-peer>"
    OMARCHY_SYSTEM_OBSERVE_HYPRCTL="$<TARGET_FILE:omarchy-plugin-package-probe-test-peer>"
    OMARCHY_SYSTEM_OBSERVE_TESTING=1)
  target_link_libraries(omarchy-plugin-system-observer-test-peer PRIVATE Qt6::Core)
  add_dependencies(omarchy-plugin-system-observer-test-peer
    omarchy-plugin-package-probe-test-peer)
  omarchy_plugin_test(plugin-system-observer COMMAND omarchy-plugin-system-observer-test
    TIMEOUT 5 LABELS "unit;security;providers;system;privacy;adversarial"
    SOURCES tests/system_observe_provider_test.cpp
    LIBRARIES Qt6::Core)
  target_compile_definitions(omarchy-plugin-system-observer-test PRIVATE
    SYSTEM_OBSERVER_PATH="$<TARGET_FILE:omarchy-plugin-system-observer-test-peer>"
    SYSTEM_OBSERVE_CONTRACT_DIGEST="${OMARCHY_SYSTEM_OBSERVE_CONTRACT_DIGEST}")
  add_dependencies(omarchy-plugin-system-observer-test
    omarchy-plugin-system-observer-test-peer)

  add_executable(omarchy-plugin-provider-peer tests/provider_peer.cpp)

  omarchy_plugin_test(plugin-provider-host COMMAND omarchy-plugin-provider-host-test
    TIMEOUT 30 LABELS "unit;security;providers;protocol;filesystem;adversarial"
    SOURCES tests/provider_host_test.cpp
    LIBRARIES omarchy-plugin-provider-host)
  target_compile_definitions(omarchy-plugin-provider-host-test PRIVATE
    PROVIDER_PEER_PATH="$<TARGET_FILE:omarchy-plugin-provider-peer>")
  add_dependencies(omarchy-plugin-provider-host-test
    omarchy-plugin-provider-peer)
  omarchy_plugin_test(plugin-provider-host-systemd-scope
    COMMAND omarchy-plugin-provider-host-test systemd
    TIMEOUT 15 SKIP_RETURN_CODE 77
    LABELS "integration;security;providers;cgroup;adversarial")
endif()
