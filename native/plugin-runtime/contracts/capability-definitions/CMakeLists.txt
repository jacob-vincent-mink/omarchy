add_library(omarchy-plugin-capability-definitions STATIC
  src/capability_definition.cpp src/capability_definition_loader.cpp
  src/dynamic_activation.cpp src/packaged_definitions.cpp src/authoring_manifest.cpp)
target_include_directories(omarchy-plugin-capability-definitions PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(omarchy-plugin-capability-definitions PUBLIC
  omarchy-plugin-permission-contract PRIVATE nlohmann_json::nlohmann_json)

add_executable(omarchy-plugin-generate-capability-catalog
  tools/generate_builtin_catalog.cpp)
target_link_libraries(omarchy-plugin-generate-capability-catalog PRIVATE
  omarchy-plugin-capability-definitions)

set(catalog_output "${CMAKE_CURRENT_BINARY_DIR}/catalog")
set(catalog_index "${catalog_output}/capability-catalog-v1.json")
set(catalog_definitions
  external.open-uri.https
  network.fetch
  system.observe
  device.observe
  device.control
  media.play-stream
  bash.execute
  storage.private
  notifications.send)
set(catalog_files)
foreach(definition IN LISTS catalog_definitions)
  list(APPEND catalog_files
    "${catalog_output}/capabilities.d/${definition}.capability")
endforeach()
add_custom_command(
  OUTPUT ${catalog_index} ${catalog_files}
  COMMAND omarchy-plugin-generate-capability-catalog "${catalog_output}"
  DEPENDS omarchy-plugin-generate-capability-catalog
  VERBATIM)
add_custom_target(omarchy-plugin-capability-catalog ALL
  DEPENDS ${catalog_index} ${catalog_files})
install(FILES ${catalog_index}
  DESTINATION "${OMARCHY_PLUGIN_INSTALL_ROOT}/metadata")
install(FILES ${catalog_files}
  DESTINATION "${OMARCHY_PLUGIN_INSTALL_ROOT}/capabilities.d")

if(BUILD_TESTING)
  omarchy_plugin_test(plugin-authoring-manifest-contract
    COMMAND omarchy-plugin-authoring-manifest-test
    TIMEOUT 15 LABELS "unit;security;adversarial"
    SOURCES tests/authoring_manifest_test.cpp
    LIBRARIES omarchy-plugin-capability-definitions nlohmann_json::nlohmann_json)

  omarchy_plugin_test(capability-definition-contract
    COMMAND omarchy-plugin-capability-definition-test
    LABELS "security"
    SOURCES tests/capability_definition_test.cpp tests/capability_definition_loader_test.cpp
      tests/permissions_extensibility_contract_test.cpp
    LIBRARIES omarchy-plugin-capability-definitions)

  add_test(NAME capability-catalog-determinism
    COMMAND "${CMAKE_COMMAND}"
      -DGENERATOR=$<TARGET_FILE:omarchy-plugin-generate-capability-catalog>
      -DGOLDEN_INDEX=${CMAKE_CURRENT_SOURCE_DIR}/tests/capability_catalog_golden.json
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/catalog-repeat
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_catalog.cmake)
endif()
