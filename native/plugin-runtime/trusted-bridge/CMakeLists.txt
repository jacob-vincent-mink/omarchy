find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick)
qt_standard_project_setup()

add_library(omarchy-plugin-trusted-bridge STATIC
  src/TrustedInputAuthority.cpp
  src/TrustedInputAuthority.h
  src/pointer_provenance.hpp
  src/remote_surface.cpp
  src/remote_surface.hpp
  src/render_input_transport.cpp
  src/render_input_transport.hpp
)
set_target_properties(omarchy-plugin-trusted-bridge PROPERTIES
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(omarchy-plugin-trusted-bridge PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(omarchy-plugin-trusted-bridge PUBLIC
  Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick omarchy-plugin-surface-contract
)

if(BUILD_TESTING)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  omarchy_plugin_test(plugin-trusted-bridge COMMAND omarchy-plugin-trusted-bridge-test
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME="
    LABELS "unit;integration;adversarial;render;qml;security"
    SOURCES tests/trusted_bridge_test.cpp
    LIBRARIES omarchy-plugin-trusted-bridge Qt6::Gui Qt6::Quick Qt6::Test)
endif()
