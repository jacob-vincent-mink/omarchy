omarchy_plugin_test(plugin-runtime-protocol-version COMMAND omarchy-plugin-runtime-contract-test protocol
  LABELS "unit;protocol" TIMEOUT 10
  DEFAULTS SOURCES runtime_contract_test.cpp
  LIBRARIES Qt6::Core Qt6::Qml Omarchy::PluginRuntimeVersion)


if(TARGET omarchy-plugin-qml-worker)
  omarchy_plugin_test(plugin-runtime-version-reporting
    COMMAND omarchy-plugin-runtime-contract-test version
      "$<TARGET_FILE:omarchy-plugin-qml-worker>"
    LABELS "integration" TIMEOUT 10)

  omarchy_plugin_test(plugin-runtime-direct-worker-denial
    COMMAND omarchy-plugin-runtime-contract-test worker-denial "$<TARGET_FILE:omarchy-plugin-qml-worker>"
    LABELS "adversarial" TIMEOUT 10)
endif()

omarchy_plugin_test(plugin-runtime-bridge-import
  COMMAND omarchy-plugin-runtime-contract-test bridge "${QT_QML_OUTPUT_DIRECTORY}"
  LABELS "integration;qml" TIMEOUT 10)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/support/CMakeLists.txt")
  add_subdirectory(support)
endif()
