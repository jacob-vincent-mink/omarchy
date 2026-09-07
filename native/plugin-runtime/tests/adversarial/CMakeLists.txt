add_executable(omarchy-plugin-malicious-worker malicious_worker.cpp)

omarchy_plugin_test(plugin-adversarial-harness
  COMMAND omarchy-plugin-adversarial-harness-test
  LABELS "unit;integration;adversarial;fixtures;linux;security"
  TIMEOUT 15
  SOURCES adversarial_harness_test.cpp
  LIBRARIES Omarchy::PluginLauncherTestSupport)
target_compile_definitions(omarchy-plugin-adversarial-harness-test PRIVATE
  MALICIOUS_WORKER_PATH="$<TARGET_FILE:omarchy-plugin-malicious-worker>"
)
add_dependencies(omarchy-plugin-adversarial-harness-test
  omarchy-plugin-malicious-worker
)
