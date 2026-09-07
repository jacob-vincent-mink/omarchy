add_library(omarchy-plugin-sandbox-policy STATIC src/policy.cpp)
add_library(Omarchy::PluginSandboxPolicy ALIAS omarchy-plugin-sandbox-policy)

omarchy_plugin_target_defaults(omarchy-plugin-sandbox-policy)
target_include_directories(omarchy-plugin-sandbox-policy PUBLIC include)
target_link_libraries(omarchy-plugin-sandbox-policy PUBLIC
  Omarchy::PluginRuntimePaths)

if(BUILD_TESTING)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SECCOMP REQUIRED IMPORTED_TARGET libseccomp)

  add_library(omarchy-plugin-sandbox-test-support INTERFACE)
  add_library(Omarchy::PluginSandboxTestSupport ALIAS
    omarchy-plugin-sandbox-test-support)
  target_include_directories(omarchy-plugin-sandbox-test-support INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests/support/include>")
  target_link_libraries(omarchy-plugin-sandbox-test-support INTERFACE
    Omarchy::PluginSandboxPolicy)

  omarchy_plugin_test(plugin-sandbox-policy COMMAND omarchy-plugin-sandbox-policy-test
    LABELS "unit;adversarial;sandbox" TIMEOUT 10
    DEFAULTS SOURCES tests/policy_test.cpp
    LIBRARIES Omarchy::PluginSandboxTestSupport)

  add_executable(omarchy-plugin-sandbox-probe tests/sandbox_probe.cpp)
  omarchy_plugin_target_defaults(omarchy-plugin-sandbox-probe)

  omarchy_plugin_test(plugin-sandbox-enforcement COMMAND omarchy-plugin-sandbox-enforcement-test
    LABELS "integration;adversarial;sandbox;security" TIMEOUT 15 SKIP_RETURN_CODE 77
    DEFAULTS SOURCES tests/enforcement_test.cpp
    LIBRARIES Omarchy::PluginSandboxTestSupport PkgConfig::SECCOMP)
  target_compile_definitions(omarchy-plugin-sandbox-enforcement-test PRIVATE
    SANDBOX_PROBE_PATH="$<TARGET_FILE:omarchy-plugin-sandbox-probe>"
  )
  add_dependencies(omarchy-plugin-sandbox-enforcement-test omarchy-plugin-sandbox-probe)
endif()
