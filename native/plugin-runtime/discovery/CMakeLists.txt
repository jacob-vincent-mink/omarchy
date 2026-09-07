if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(omarchy_plugin_discovery LANGUAGES CXX)
  include(CTest)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../contracts/manifest"
                   "${CMAKE_CURRENT_BINARY_DIR}/manifest-contract")
endif()

add_library(omarchy-plugin-discovery STATIC
  src/discovery.cpp
  src/revision_ingress.cpp)
target_include_directories(omarchy-plugin-discovery PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(omarchy-plugin-discovery PUBLIC omarchy-plugin-capability-definitions)

if(BUILD_TESTING)
  target_compile_definitions(omarchy-plugin-discovery
    PUBLIC OMARCHY_REVISION_INGRESS_TESTING)
  omarchy_plugin_test(plugin-manifest-discovery COMMAND omarchy-plugin-discovery-test
    TIMEOUT 20 LABELS "unit;integration;adversarial"
    SOURCES tests/discovery_test.cpp
    LIBRARIES omarchy-plugin-discovery)
  target_compile_definitions(omarchy-plugin-discovery-test PRIVATE
    MANIFEST_V2_FIXTURE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../contracts/manifest/tests/fixtures/valid-minimal"
    MANIFEST_DUPLICATE_FIXTURE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../contracts/manifest/tests/fixtures/invalid-duplicate"
    TREE_SHA256_GOLDEN="f08ffb620b68039ea1e856e6e2fe7501f3bc9e43d255be5f1ab6c3389b1c708d"
  )

  omarchy_plugin_test(plugin-revision-ingress
    COMMAND omarchy-plugin-revision-ingress-test
    TIMEOUT 60 LABELS "integration;adversarial;crash-consistency"
    SOURCES tests/revision_ingress_test.cpp
    LIBRARIES omarchy-plugin-discovery)
endif()
