add_library(omarchy-plugin-providers STATIC
  src/private_storage_backend.cpp
  src/local_provider.cpp
)
target_include_directories(omarchy-plugin-providers PUBLIC include)
target_link_libraries(omarchy-plugin-providers PUBLIC
  omarchy-plugin-broker-protocol
  omarchy-plugin-capability-definitions
)

if(BUILD_TESTING)

  omarchy_plugin_test(plugin-providers COMMAND omarchy-plugin-providers-test
    LABELS "unit;protocol;adversarial;providers;security"
    SOURCES tests/local_provider_test.cpp
    LIBRARIES omarchy-plugin-providers)

  omarchy_plugin_test(plugin-private-storage
    COMMAND omarchy-plugin-private-storage-test
    LABELS "unit;adversarial;providers;filesystem"
    SOURCES tests/private_storage_backend_test.cpp
    LIBRARIES omarchy-plugin-providers)
endif()
