find_package(PkgConfig REQUIRED)
# Qt has no public API for injecting a complete touch frame into a window.
# Keep the exact-version QPA dependency inside the worker adapter target.
find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
pkg_check_modules(WORKER_SECCOMP REQUIRED IMPORTED_TARGET libseccomp)

add_library(omarchy-plugin-worker-runtime STATIC
  src/plugin_source_policy.cpp
  src/qt_touch_injector.cpp
  src/qml_broker_api.cpp
  src/worker_channel.cpp
  src/worker_runtime.cpp
  src/steady_seccomp.cpp
)
set(presentation_qmldir "${CMAKE_CURRENT_SOURCE_DIR}/../presentation/qml/qmldir")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${presentation_qmldir}")
file(STRINGS "${presentation_qmldir}" presentation_entries REGEX "\\.qml$")
set(presentation_files ../presentation/qml/qmldir)
foreach(entry IN LISTS presentation_entries)
  string(REGEX REPLACE "^.* " "" filename "${entry}")
  list(APPEND presentation_files "../presentation/qml/${filename}")
endforeach()
list(SORT presentation_files)
qt_add_resources(omarchy-plugin-worker-runtime plugin_presentation_qml
  PREFIX "/qt/qml/Omarchy/PluginPresentation"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/../presentation/qml"
  FILES ${presentation_files}
)
omarchy_plugin_target_defaults(omarchy-plugin-worker-runtime)
target_include_directories(omarchy-plugin-worker-runtime PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/../contracts/wire/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../contracts/surface/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../contracts/sandbox/include"
)
target_link_libraries(omarchy-plugin-worker-runtime
  PUBLIC Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
         omarchy-plugin-wire omarchy-plugin-surface-contract
         omarchy-plugin-broker-protocol omarchy-plugin-capability-definitions
  PRIVATE Qt6::GuiPrivate
          omarchy-plugin-sandbox-policy omarchy-plugin-manifest-contract
          PkgConfig::WORKER_SECCOMP
)

qt_add_executable(omarchy-plugin-qml-worker src/main.cpp)

qt_add_qml_module(omarchy-plugin-qml-worker
  URI Omarchy.PluginWorker
  VERSION 1.0
  RESOURCE_PREFIX /qt/qml
  QML_FILES Runtime.qml
)

omarchy_plugin_target_defaults(omarchy-plugin-qml-worker)
target_link_libraries(omarchy-plugin-qml-worker
  PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Omarchy::PluginRuntimeVersion
          Omarchy::PluginRuntimePaths
          omarchy-plugin-worker-runtime
)

install(TARGETS omarchy-plugin-qml-worker RUNTIME DESTINATION "${OMARCHY_PLUGIN_INSTALL_ROOT}/bin")

if(BUILD_TESTING)
  omarchy_plugin_test(plugin-worker-runtime COMMAND omarchy-plugin-worker-runtime-test
    LABELS "unit;integration;adversarial;render;security"
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=none;QT_QUICK_CONTROLS_STYLE=Basic;QSG_RHI_BACKEND=software;QSG_SOFTWARE_RENDERER_FORCE_PARTIAL_UPDATES=0"
    DEFAULTS SOURCES tests/worker_runtime_test.cpp
    LIBRARIES omarchy-plugin-worker-runtime)
  target_compile_definitions(omarchy-plugin-worker-runtime-test PRIVATE
    WORKER_FIXTURE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
  )
  add_test(NAME plugin-worker-runtime-sibling-focus
    COMMAND omarchy-plugin-worker-runtime-test --sibling-focus-only)
  add_test(NAME plugin-worker-runtime-sibling-grab
    COMMAND omarchy-plugin-worker-runtime-test --sibling-grab-only)
  set_tests_properties(
    plugin-worker-runtime-sibling-focus
    plugin-worker-runtime-sibling-grab
    PROPERTIES
      LABELS "unit;integration;render;input"
      TIMEOUT 20
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=none;QT_QUICK_CONTROLS_STYLE=Basic;QSG_RHI_BACKEND=software;QSG_SOFTWARE_RENDERER_FORCE_PARTIAL_UPDATES=0"
  )

  omarchy_plugin_test(plugin-worker-channel COMMAND omarchy-plugin-worker-channel-test
    LABELS "unit;protocol;adversarial;security"
    TIMEOUT 20
    DEFAULTS SOURCES tests/worker_channel_test.cpp
    LIBRARIES omarchy-plugin-worker-runtime Omarchy::PluginTestSupport)

  omarchy_plugin_test(plugin-qml-broker-api COMMAND omarchy-plugin-qml-broker-api-test
    LABELS "unit;integration;protocol;broker;qml;security"
    TIMEOUT 20
    SKIP_RETURN_CODE 77
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=none;QT_QUICK_CONTROLS_STYLE=Basic;QSG_RHI_BACKEND=software"
    DEFAULTS SOURCES tests/qml_broker_api_test.cpp
    LIBRARIES omarchy-plugin-worker-runtime omarchy-plugin-host-session Omarchy::PluginTestSupport)
  target_compile_definitions(omarchy-plugin-qml-broker-api-test PRIVATE
    OMARCHY_NEUTRAL_SURFACE_FIXTURE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/neutral-surfaces"
  )
endif()
