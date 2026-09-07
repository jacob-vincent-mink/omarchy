add_library(omarchy-plugin-permission-contract STATIC
  src/audit_contract.cpp
)
target_include_directories(omarchy-plugin-permission-contract PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(omarchy-plugin-permission-contract PUBLIC
  omarchy-plugin-manifest-contract
)

if(BUILD_TESTING)
  omarchy_plugin_test(plugin-permission-contract
           COMMAND omarchy-plugin-permission-contract-test
    TIMEOUT 15
    LABELS "unit;protocol;adversarial;security"
    SOURCES tests/permission_contract_test.cpp
    LIBRARIES omarchy-plugin-permission-contract)
endif()
