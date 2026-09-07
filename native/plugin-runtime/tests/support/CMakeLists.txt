add_library(omarchy-plugin-test-support STATIC src/test_support.cpp)
add_library(Omarchy::PluginTestSupport ALIAS omarchy-plugin-test-support)
omarchy_plugin_target_defaults(omarchy-plugin-test-support)
target_include_directories(omarchy-plugin-test-support PUBLIC include)

omarchy_plugin_test(plugin-test-support COMMAND omarchy-plugin-support-test
  LABELS "unit;fixtures" TIMEOUT 10
  DEFAULTS SOURCES tests/support_test.cpp
  LIBRARIES Omarchy::PluginTestSupport)
option(PLUGIN_SECURITY_BUILD_FUZZERS "Build bounded libFuzzer targets" OFF)
if(PLUGIN_SECURITY_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "PLUGIN_SECURITY_BUILD_FUZZERS requires Clang")
  endif()

  set(plugin_security_fuzzers)
  add_executable(omarchy-plugin-envelope-fuzzer fuzz/fuzz_envelope.cpp)
  target_link_libraries(omarchy-plugin-envelope-fuzzer PRIVATE omarchy-plugin-wire)
  list(APPEND plugin_security_fuzzers omarchy-plugin-envelope-fuzzer)
  add_executable(omarchy-plugin-manifest-fuzzer fuzz/fuzz_manifest.cpp)
  target_link_libraries(omarchy-plugin-manifest-fuzzer PRIVATE omarchy-plugin-manifest-contract)
  list(APPEND plugin_security_fuzzers omarchy-plugin-manifest-fuzzer)
  add_executable(omarchy-plugin-render-fuzzer fuzz/fuzz_render.cpp)
  target_link_libraries(omarchy-plugin-render-fuzzer PRIVATE omarchy-plugin-surface-contract)
  list(APPEND plugin_security_fuzzers omarchy-plugin-render-fuzzer)

  foreach(target IN LISTS plugin_security_fuzzers)
    omarchy_plugin_target_defaults(${target})
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
  endforeach()
  add_custom_target(plugin-security-fuzzers DEPENDS ${plugin_security_fuzzers})
endif()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../launcher/tests" launcher-tests)

add_executable(omarchy-plugin-t11-harness t11_power_harness.cpp)
target_link_libraries(omarchy-plugin-t11-harness PRIVATE omarchy-plugin-test-support omarchy-plugin-activation-snapshot)

