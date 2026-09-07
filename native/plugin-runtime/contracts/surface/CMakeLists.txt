add_library(omarchy-plugin-surface-contract STATIC
  src/frame_region.cpp
  src/frame_transport.cpp
  src/input.cpp
  src/render_messages.cpp
  src/shared_layout.cpp
  src/surface_state.cpp
)
set_target_properties(omarchy-plugin-surface-contract PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(omarchy-plugin-surface-contract PUBLIC include)
target_link_libraries(omarchy-plugin-surface-contract PUBLIC omarchy-plugin-wire PRIVATE Qt6::Core)

if(BUILD_TESTING)
  foreach(test_name IN ITEMS layout frame_region boundary publication surface_state input profile)
    omarchy_plugin_test(plugin-surface-${test_name} COMMAND plugin-surface-${test_name}-test
      LABELS "unit;render"
      SOURCES tests/${test_name}_test.cpp
      LIBRARIES omarchy-plugin-surface-contract)
  endforeach()
  set_tests_properties(plugin-surface-publication PROPERTIES LABELS "adversarial;property;render")
  set_tests_properties(plugin-surface-frame_region PROPERTIES LABELS "adversarial;integration;render")
  set_tests_properties(plugin-surface-layout PROPERTIES LABELS "property;render")
  set_tests_properties(plugin-surface-input PROPERTIES LABELS "adversarial;render")
  set_tests_properties(plugin-surface-boundary PROPERTIES LABELS "adversarial;integration;render;security")
  set_tests_properties(plugin-surface-profile PROPERTIES LABELS "integration;protocol;render")
endif()
