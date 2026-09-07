find_package(Qt6 REQUIRED COMPONENTS Core DBus)

add_library(omarchy-plugin-channel-integration STATIC
  src/authenticated_channel.cpp
)
target_include_directories(omarchy-plugin-channel-integration PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
omarchy_plugin_target_defaults(omarchy-plugin-channel-integration)
target_link_libraries(omarchy-plugin-channel-integration PUBLIC
  Omarchy::PluginLauncher
  omarchy-plugin-broker-protocol
  omarchy-plugin-surface-contract
)

# Standalone builds validate the low transport target above. The native runtime
# root supplies host-session and is the authenticated composition build owner.
if(TARGET omarchy-plugin-host-session)
  set(authenticated_session_sources
    src/plugin_session_io.cpp
    src/authenticated_session_channel.cpp
    src/desktop_notification_service.cpp
    src/plugin_permission_authority.cpp
    src/runtime_bootstrap.cpp
    src/activation_catalog.cpp
    src/runtime_roots.cpp
    src/plugin_session_activation.cpp
    src/session_runtime_factory.cpp
    src/plugin_session.cpp
    src/runtime_session_owner.cpp
    src/runtime_session_catalog.cpp
    src/runtime_session_permissions.cpp
    src/runtime_session_surfaces.cpp
  )
  set(authenticated_session_targets omarchy-plugin-authenticated-session)
  if(BUILD_TESTING)
    list(APPEND authenticated_session_targets omarchy-plugin-authenticated-session-test-support)
  endif()
  foreach(target IN LISTS authenticated_session_targets)
    add_library(${target} STATIC ${authenticated_session_sources})
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    omarchy_plugin_target_defaults(${target})
    target_link_libraries(${target} PUBLIC
      omarchy-plugin-channel-integration omarchy-plugin-activation-snapshot
      omarchy-plugin-host-session omarchy-plugin-discovery
      omarchy-plugin-provider-host omarchy-plugin-providers)
    target_link_libraries(${target} PRIVATE Qt6::Core Qt6::DBus)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(BUILD_TESTING)
      target_compile_definitions(${target} PUBLIC OMARCHY_PLUGIN_PERMISSION_AUTHORITY_TESTING)
    endif()
  endforeach()
  if(BUILD_TESTING)
    # Every object and consumer must agree on test-only class layouts. Never
    # splice individually instrumented sources into the production library.
    target_compile_definitions(omarchy-plugin-authenticated-session-test-support PUBLIC
      OMARCHY_PLUGIN_SESSION_TESTING OMARCHY_RUNTIME_BOOTSTRAP_TESTING OMARCHY_RUNTIME_ROOTS_TESTING)
    target_link_libraries(omarchy-plugin-authenticated-session-test-support PUBLIC Omarchy::PluginLauncherTestSupport)
  endif()
endif()

if(BUILD_TESTING)
  add_executable(omarchy-plugin-channel-peer tests/helpers/channel_peer.cpp)
  omarchy_plugin_target_defaults(omarchy-plugin-channel-peer)
  target_link_libraries(omarchy-plugin-channel-peer PRIVATE
    omarchy-plugin-broker-protocol
    omarchy-plugin-surface-contract
  )

  add_executable(omarchy-plugin-channel-fake-bwrap ../launcher/tests/fake_bwrap.cpp)
  target_compile_definitions(omarchy-plugin-channel-fake-bwrap PRIVATE
    OMARCHY_CHANNEL_FAKE_BWRAP=1)
  omarchy_plugin_target_defaults(omarchy-plugin-channel-fake-bwrap)

  omarchy_plugin_test(plugin-channel-integration-fake
    COMMAND omarchy-plugin-channel-integration-test fake
    TIMEOUT 30
    LABELS "unit;integration;adversarial;protocol;broker;launcher"
    DEFAULTS SOURCES tests/authenticated_channel_test.cpp
    LIBRARIES omarchy-plugin-channel-integration Omarchy::PluginLauncherTestSupport Omarchy::PluginTestSupport)
  target_compile_definitions(omarchy-plugin-channel-integration-test PRIVATE
    CHANNEL_PEER_PATH="$<TARGET_FILE:omarchy-plugin-channel-peer>"
    FAKE_BWRAP_PATH="$<TARGET_FILE:omarchy-plugin-channel-fake-bwrap>"
    BWRAP_PATH="/usr/bin/bwrap"
  )
  add_dependencies(omarchy-plugin-channel-integration-test
    omarchy-plugin-channel-peer
    omarchy-plugin-channel-fake-bwrap
  )

  omarchy_plugin_test(plugin-channel-integration-bwrap
    COMMAND omarchy-plugin-channel-integration-test bwrap
    TIMEOUT 30
    SKIP_RETURN_CODE 77
    LABELS "integration;adversarial;protocol;broker;launcher;sandbox;linux;security")

  if(TARGET omarchy-plugin-authenticated-session)
    omarchy_plugin_test(plugin-runtime-session-owner
      COMMAND omarchy-plugin-runtime-session-owner-test
      TIMEOUT 10 LABELS "unit;security;session;lifecycle;ownership"
      DEFAULTS SOURCES tests/runtime_session_owner_test.cpp
      LIBRARIES omarchy-plugin-authenticated-session-test-support Qt6::Core Omarchy::PluginTestSupport)

    omarchy_plugin_test(plugin-product-session-state
      COMMAND omarchy-plugin-product-session-state-test
      TIMEOUT 10 LABELS "unit;security;session;lifecycle"
      DEFAULTS SOURCES tests/product_session_state_test.cpp
      LIBRARIES omarchy-plugin-authenticated-session Omarchy::PluginTestSupport)

    add_executable(omarchy-plugin-provider-composition-peer
      tests/helpers/provider_composition_peer.cpp)
    omarchy_plugin_target_defaults(omarchy-plugin-provider-composition-peer)

    omarchy_plugin_test(plugin-provider-composition
      COMMAND omarchy-plugin-provider-composition-test
      TIMEOUT 30
      LABELS "integration;security;providers;bootstrap;protocol;filesystem;adversarial"
      DEFAULTS SOURCES tests/provider_composition_test.cpp
      LIBRARIES omarchy-plugin-authenticated-session-test-support Qt6::DBus)
    target_compile_definitions(omarchy-plugin-provider-composition-test PRIVATE
      PROVIDER_COMPOSITION_PEER_PATH="$<TARGET_FILE:omarchy-plugin-provider-composition-peer>")
    add_dependencies(omarchy-plugin-provider-composition-test
      omarchy-plugin-provider-composition-peer)
    find_program(OMARCHY_FAKEROOT_EXECUTABLE fakeroot)
    if(OMARCHY_FAKEROOT_EXECUTABLE AND
       NOT OMARCHY_PLUGIN_SANITIZERS)
      omarchy_plugin_test(plugin-provider-composition-ownership
        COMMAND "${OMARCHY_FAKEROOT_EXECUTABLE}" -u "${CMAKE_COMMAND}" -E env
          OMARCHY_TEST_FAKE_OWNERSHIP=1
          "$<TARGET_FILE:omarchy-plugin-provider-composition-test>"
        TIMEOUT 30
        LABELS "integration;security;providers;bootstrap;filesystem;adversarial;ownership")
    endif()

    omarchy_plugin_test(plugin-desktop-notification-service
      COMMAND omarchy-plugin-desktop-notification-service-test
      TIMEOUT 10 LABELS "unit;security;providers;dbus"
      DEFAULTS SOURCES tests/desktop_notification_service_test.cpp
      LIBRARIES omarchy-plugin-authenticated-session Qt6::Core Qt6::DBus)

    omarchy_plugin_test(plugin-activation-catalog
      COMMAND omarchy-plugin-activation-catalog-test
      TIMEOUT 30 LABELS "unit;security;catalog;filesystem;adversarial"
      DEFAULTS SOURCES src/activation_catalog.cpp tests/activation_catalog_test.cpp
      LIBRARIES omarchy-plugin-activation-snapshot)

    omarchy_plugin_test(plugin-runtime-roots
      COMMAND omarchy-plugin-runtime-roots-test
      TIMEOUT 30 LABELS "unit;security;catalog;filesystem;adversarial"
      DEFAULTS SOURCES src/runtime_roots.cpp src/activation_catalog.cpp tests/runtime_roots_test.cpp
      LIBRARIES omarchy-plugin-activation-snapshot omarchy-plugin-discovery
        omarchy-plugin-authenticated-session Omarchy::PluginTestSupport)
    target_compile_definitions(omarchy-plugin-runtime-roots-test PRIVATE
      OMARCHY_RUNTIME_ROOTS_TESTING)

    omarchy_plugin_test(plugin-runtime-bootstrap
      COMMAND omarchy-plugin-runtime-bootstrap-test
      TIMEOUT 30 LABELS "unit;security;bootstrap;filesystem;adversarial"
      DEFAULTS SOURCES tests/runtime_bootstrap_test.cpp
      LIBRARIES omarchy-plugin-authenticated-session-test-support Qt6::DBus Omarchy::PluginLauncherTestSupport)
    target_compile_definitions(omarchy-plugin-runtime-bootstrap-test PRIVATE
      QML_WORKER_PATH="$<TARGET_FILE:omarchy-plugin-qml-worker>")
    add_dependencies(omarchy-plugin-runtime-bootstrap-test omarchy-plugin-qml-worker)
    omarchy_plugin_test(plugin-author-archive-real-worker
      COMMAND omarchy-plugin-runtime-bootstrap-test --author-worker-only
      TIMEOUT 30 LABELS "integration;security;bootstrap;qml;sandbox;authoring"
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    omarchy_plugin_test(plugin-broker-session-settlement
      COMMAND omarchy-plugin-broker-session-settlement-test
      TIMEOUT 30 LABELS "unit;security;session;broker;adversarial"
      DEFAULTS SOURCES tests/broker_session_settlement_test.cpp
      LIBRARIES omarchy-plugin-channel-integration omarchy-plugin-host-session omarchy-plugin-providers)

    omarchy_plugin_test(plugin-product-session
      COMMAND omarchy-plugin-product-session-test
      TIMEOUT 30 LABELS "unit;security;session;surface"
      DEFAULTS SOURCES tests/session_runtime_factory_test.cpp tests/plugin_session_test.cpp
      LIBRARIES omarchy-plugin-authenticated-session-test-support Omarchy::PluginLauncherTestSupport Qt6::Core
        omarchy-plugin-activation-snapshot omarchy-plugin-provider-host omarchy-plugin-providers)
    target_compile_definitions(omarchy-plugin-product-session-test PRIVATE
      CHANNEL_PEER_PATH="$<TARGET_FILE:omarchy-plugin-channel-peer>"
      PROVIDER_COMPOSITION_PEER_PATH="$<TARGET_FILE:omarchy-plugin-provider-composition-peer>"
      FAKE_BWRAP_PATH="$<TARGET_FILE:omarchy-plugin-channel-fake-bwrap>"
      QML_WORKER_PATH="$<TARGET_FILE:omarchy-plugin-qml-worker>"
      RUNTIME_ROOT_REVISION_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../contracts/manifest/tests/fixtures/valid-minimal")
    add_dependencies(omarchy-plugin-product-session-test
      omarchy-plugin-channel-peer omarchy-plugin-provider-composition-peer
      omarchy-plugin-channel-fake-bwrap
      omarchy-plugin-qml-worker)
    omarchy_plugin_test(plugin-product-session-provider-settlement
      COMMAND omarchy-plugin-product-session-test --provider-settlement-only
      TIMEOUT 15
      LABELS "integration;security;session;providers;protocol")
    omarchy_plugin_test(plugin-session-channel-boundaries
      COMMAND omarchy-plugin-product-session-test --channel-boundaries-only
      TIMEOUT 30 LABELS "integration;security;session;protocol;adversarial")
    omarchy_plugin_test(plugin-session-io
      COMMAND omarchy-plugin-product-session-test --session-io-only
      TIMEOUT 30 LABELS "integration;security;session;adversarial")
    omarchy_plugin_test(plugin-product-session-real-worker-invalid-qml
      COMMAND omarchy-plugin-product-session-test
              --real-worker-invalid-qml-only
      TIMEOUT 15
      SKIP_RETURN_CODE 77
      LABELS "integration;security;session;qml;sandbox"
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    omarchy_plugin_test(plugin-product-session-real-worker-ordered-startup
      COMMAND omarchy-plugin-product-session-test
              --real-worker-valid-qml-only
      TIMEOUT 15
      SKIP_RETURN_CODE 77
      LABELS "integration;security;session;qml;sandbox;startup"
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    omarchy_plugin_test(plugin-consent-only-classification
      COMMAND omarchy-plugin-product-session-test
              --consent-only-classification-only
      TIMEOUT 15
      LABELS "unit;security;session;permissions;lifecycle")
    omarchy_plugin_test(plugin-product-session-required-denial
      COMMAND omarchy-plugin-product-session-test --required-denial-only
      TIMEOUT 15 LABELS "unit;security;session;authority")
    omarchy_plugin_test(plugin-runtime-root-fences
      COMMAND omarchy-plugin-product-session-test --runtime-root-fence-only
      TIMEOUT 15 LABELS "unit;security;session;authority")
  endif()
endif()
