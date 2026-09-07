set(plugin_host_sources
  src/PermissionControl.cpp
  src/PluginInstallControl.cpp
  src/PluginManager.cpp
  src/PluginRuntimeController.cpp
  src/SurfaceEndpoint.cpp
  src/SurfaceEndpointOwner.cpp
  src/SurfaceProjectionModel.cpp
)

set(plugin_host_module_dir "${QT_QML_OUTPUT_DIRECTORY}/Omarchy/PluginHost")

qt_add_qml_module(omarchy-plugin-host-bridge
  SHARED
  URI Omarchy.PluginHost
  VERSION 1.0
  PLUGIN_TARGET omarchy-plugin-host-bridge
  OUTPUT_DIRECTORY "${plugin_host_module_dir}"
  SOURCES
    ${plugin_host_sources}
    src/PermissionControl.h
    src/PluginInstallControl.h
    src/PluginManager.h
    src/PluginRuntimeController.h
    src/SurfaceEndpoint.h
    src/SurfaceEndpointOwner.h
    src/SurfaceProjectionModel.h
    src/runtime_contract.cpp
    ../trusted-bridge/src/remote_surface.hpp
)

omarchy_plugin_target_defaults(omarchy-plugin-host-bridge)
set_target_properties(omarchy-plugin-host-bridge PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)
target_link_options(omarchy-plugin-host-bridge PRIVATE
  "LINKER:--exclude-libs,ALL")
target_include_directories(omarchy-plugin-host-bridge PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/../trusted-bridge/src"
)
target_link_libraries(omarchy-plugin-host-bridge PRIVATE
  Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
  Omarchy::PluginRuntimeVersion
  Omarchy::PluginRuntimePaths
  omarchy-plugin-trusted-bridge
  omarchy-plugin-host-session
  omarchy-plugin-authenticated-session
  omarchy-plugin-surface-host
  omarchy-plugin-surface-contract
  omarchy-plugin-wire
)

install(TARGETS omarchy-plugin-host-bridge LIBRARY DESTINATION "${OMARCHY_PLUGIN_INSTALL_ROOT}/qml/Omarchy/PluginHost")
install(FILES
  "${plugin_host_module_dir}/qmldir"
  "${plugin_host_module_dir}/omarchy-plugin-host-bridge.qmltypes"
  DESTINATION "${OMARCHY_PLUGIN_INSTALL_ROOT}/qml/Omarchy/PluginHost"
)

if(BUILD_TESTING)
  add_library(omarchy-plugin-host-test-support STATIC
    ${plugin_host_sources}
    tests/PluginManagerTestAccess.cpp
  )
  omarchy_plugin_target_defaults(omarchy-plugin-host-test-support)
  target_compile_definitions(omarchy-plugin-host-test-support PUBLIC
    OMARCHY_PLUGIN_MANAGER_TESTING
    OMARCHY_SURFACE_ENDPOINT_TESTING
    OMARCHY_SURFACE_ENDPOINT_OWNER_TESTING)
  target_include_directories(omarchy-plugin-host-test-support PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../trusted-bridge/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../channel-integration/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../channel-integration/tests"
  )
  target_link_libraries(omarchy-plugin-host-test-support PUBLIC
    Qt6::Core Qt6::DBus Qt6::Gui Qt6::Qml Qt6::Quick
    omarchy-plugin-trusted-bridge
    omarchy-plugin-host-session
    omarchy-plugin-authenticated-session-test-support
    omarchy-plugin-surface-host
  )
  omarchy_plugin_test(plugin-surface-service
    COMMAND omarchy-plugin-surface-service-test
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=none;QSG_RHI_BACKEND=software"
    LABELS "unit;shell;bridge;security"
    DEFAULTS SOURCES tests/PluginManager_test.cpp tests/SurfaceEndpoint_test.cpp tests/SurfaceEndpointOwner_test.cpp
      tests/SurfaceProjectionModel_test.cpp
    LIBRARIES omarchy-plugin-host-test-support Omarchy::PluginTestSupport)
  target_compile_definitions(omarchy-plugin-surface-service-test PRIVATE
    QML_WORKER_PATH="$<TARGET_FILE:omarchy-plugin-qml-worker>"
    OMARCHY_NEUTRAL_SURFACE_FIXTURE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/neutral-surfaces")
  add_dependencies(omarchy-plugin-surface-service-test omarchy-plugin-qml-worker)
  omarchy_plugin_test(plugin-permission-control-contract
    COMMAND omarchy-plugin-permission-control-contract-test
    TIMEOUT 10
    SKIP_RETURN_CODE 77
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME="
    LABELS "unit;shell;bridge;security;permission-contract"
    DEFAULTS SOURCES tests/PermissionControl_contract_test.cpp
    LIBRARIES omarchy-plugin-host-test-support)
  target_include_directories(omarchy-plugin-permission-control-contract-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../host-session/tests"
  )

  omarchy_plugin_test(plugin-public-permission-lifecycle
    COMMAND omarchy-plugin-surface-service-test
            --public-permission-lifecycle-only
    TIMEOUT 30
    SKIP_RETURN_CODE 77
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=none;QSG_RHI_BACKEND=software"
    LABELS "integration;shell;bridge;security;permissions;lifecycle")

  omarchy_plugin_test(plugin-neutral-surfaces-real-bwrap
    COMMAND omarchy-plugin-surface-service-test
            --neutral-surfaces-real-bwrap-only
    TIMEOUT 30
    SKIP_RETURN_CODE 77
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=none;QSG_RHI_BACKEND=software"
    LABELS "integration;shell;bridge;security;sandbox;real-bwrap;fixtures")

  # T07 gesture-provenance interactive lane. Requires a real display and a
  # human gesture, so it is always skipped in the automated suite (needs
  # OMARCHY_T07_LIVE=1 on a visible session; the test binary returns 77
  # otherwise). It verifies a real compositor gesture ingresses as
  # decision=accepted + trusted-physical=true on the host-input audit line.
  omarchy_plugin_test(plugin-gesture-provenance-live
    COMMAND omarchy-plugin-surface-service-test --t07-live-only
    TIMEOUT 300
    SKIP_RETURN_CODE 77
    LABELS "manual;interactive;shell;bridge;security;gesture;real-display")
  set_tests_properties(plugin-surface-service plugin-permission-control-contract
    plugin-public-permission-lifecycle plugin-neutral-surfaces-real-bwrap
    plugin-gesture-provenance-live PROPERTIES
    ENVIRONMENT_MODIFICATION "QT_IM_MODULE=set:compose")
endif()
