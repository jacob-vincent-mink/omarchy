add_library(omarchy-plugin-broker-protocol STATIC
  src/broker_schema.cpp
)
target_include_directories(omarchy-plugin-broker-protocol PUBLIC include)
target_link_libraries(omarchy-plugin-broker-protocol PUBLIC
  omarchy-plugin-wire
  omarchy-plugin-permission-contract
)

if(BUILD_TESTING)
  omarchy_plugin_test(plugin-broker-schema COMMAND omarchy-plugin-broker-schema-test
    LABELS "unit;protocol;security;adversarial"
    SOURCES tests/broker_schema_test.cpp
    LIBRARIES omarchy-plugin-broker-protocol)
endif()
