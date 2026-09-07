add_library(omarchy-plugin-wire STATIC
  src/common.cpp
  src/envelope.cpp
  src/permission_snapshot.cpp
  src/version.cpp
)
add_library(Omarchy::PluginRuntimeVersion ALIAS omarchy-plugin-wire)
set_target_properties(omarchy-plugin-wire PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(omarchy-plugin-wire PUBLIC include)
target_compile_definitions(omarchy-plugin-wire PRIVATE
  OMARCHY_PLUGIN_BUILD_VERSION="${PROJECT_VERSION}")

if(BUILD_TESTING)
  omarchy_plugin_test(plugin-wire-contract COMMAND omarchy-plugin-wire-contract-test
    LABELS "unit;protocol;property;security"
    SOURCES tests/wire_contract_test.cpp
    LIBRARIES omarchy-plugin-wire)
endif()
