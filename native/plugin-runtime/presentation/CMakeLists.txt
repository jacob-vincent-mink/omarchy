if(BUILD_TESTING)
  omarchy_plugin_test(plugin-presentation-authority-boundary
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/presentation_boundary_test.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}/qml"
    TIMEOUT 5 LABELS "qml;presentation;security;api-snapshot")
endif()
