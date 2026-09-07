add_library(omarchy-plugin-manifest-contract STATIC src/manifest_contract.cpp src/manifest_json.cpp)
target_link_libraries(omarchy-plugin-manifest-contract PUBLIC Qt6::Core PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(omarchy-plugin-manifest-contract PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/../wire/include")

if(BUILD_TESTING)
  omarchy_plugin_test(plugin-manifest-v2-contract COMMAND omarchy-plugin-manifest-contract-test
    TIMEOUT 15
    LABELS "unit;protocol;adversarial;security"
    SOURCES tests/manifest_contract_test.cpp
    LIBRARIES omarchy-plugin-manifest-contract)
  target_compile_definitions(omarchy-plugin-manifest-contract-test PRIVATE
    MANIFEST_FIXTURE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    TREE_SHA256_GOLDEN="f08ffb620b68039ea1e856e6e2fe7501f3bc9e43d255be5f1ab6c3389b1c708d"
    MANIFEST_SHA256_GOLDEN="078bcef5be2d31f2c7b9c0c6ff847183a5d912c4349339823a77c04f7efe3cff"
    REQUEST_SHA256_GOLDEN="1818a2184c2a99c8303b595adee00eb06daeeec529c3750a2ec811580e32c318"
  )

endif()
