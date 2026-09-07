find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick)
qt_standard_project_setup()
qt_policy(SET QTP0001 NEW)

add_library(omarchy-plugin-surface-host STATIC
  src/render_session.cpp
  src/surface_host.cpp)
target_include_directories(omarchy-plugin-surface-host PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(omarchy-plugin-surface-host PUBLIC
  Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
  omarchy-plugin-manifest-contract
  omarchy-plugin-permission-contract
  omarchy-plugin-surface-contract
  omarchy-plugin-trusted-bridge
  omarchy-plugin-wire)

if(BUILD_TESTING)
  find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)

  omarchy_plugin_test(plugin-surface-host COMMAND omarchy-plugin-surface-host-test
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME="
    LABELS "unit;integration;adversarial;render;qml"
    SOURCES tests/surface_host_test.cpp
    LIBRARIES omarchy-plugin-surface-host)

  omarchy_plugin_test(plugin-render-session
    COMMAND omarchy-plugin-surface-host-render-session-test
    TIMEOUT 30
    LABELS "integration;adversarial;render;qml;protocol;security"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=none;QSG_RHI_BACKEND=software;QSG_SOFTWARE_RENDERER_FORCE_PARTIAL_UPDATES=0"
    SOURCES tests/render_session_test.cpp
    LIBRARIES omarchy-plugin-surface-host omarchy-plugin-worker-runtime Qt6::Core Qt6::Gui Qt6::GuiPrivate
      Qt6::Qml Qt6::Quick)
  set_target_properties(omarchy-plugin-surface-host-render-session-test PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
  target_include_directories(omarchy-plugin-surface-host-render-session-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../worker/src")
  target_compile_definitions(omarchy-plugin-surface-host-render-session-test PRIVATE
    SURFACE_HOST_WORKER_FIXTURE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../worker/tests/fixtures")
endif()
