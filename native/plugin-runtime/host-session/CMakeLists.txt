find_package(SQLite3 3.37 REQUIRED)
if(NOT TARGET SQLite3::SQLite3)
  add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()

add_library(omarchy-plugin-authority-database STATIC src/sqlite_authority_database.cpp)
target_include_directories(omarchy-plugin-authority-database PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(omarchy-plugin-authority-database PUBLIC SQLite3::SQLite3)
set_target_properties(omarchy-plugin-authority-database PROPERTIES
  POSITION_INDEPENDENT_CODE ON AUTOMOC OFF AUTOUIC OFF)

add_library(omarchy-plugin-activation-snapshot STATIC
  src/activation_snapshot.cpp
  src/authority_snapshot_codec.cpp
  src/authority_migration.cpp
  src/authority_sql.cpp
  src/authority_store.cpp
  src/consent_review.cpp
  src/permission_projection.cpp
  src/revision_verifier_adapter.cpp)
target_include_directories(omarchy-plugin-activation-snapshot PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(omarchy-plugin-activation-snapshot PUBLIC
  omarchy-plugin-authority-database
  omarchy-plugin-permission-contract
  omarchy-plugin-capability-definitions
  omarchy-plugin-discovery
  omarchy-plugin-wire)
if(BUILD_TESTING)
  target_compile_definitions(omarchy-plugin-activation-snapshot PUBLIC
    OMARCHY_AUTHORITY_STORE_TESTING)
endif()

add_library(omarchy-plugin-host-session STATIC
  ../audit/src/audit_store.cpp
  src/gesture_eligibility.cpp
  src/gesture_intent.cpp
  src/MultiSurfaceRouter.cpp
  src/structured_broker.cpp)
target_include_directories(omarchy-plugin-host-session PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/../audit/src")
target_link_libraries(omarchy-plugin-host-session PUBLIC
  Qt6::Core
  omarchy-plugin-surface-contract
  omarchy-plugin-broker-protocol
  omarchy-plugin-capability-definitions
  omarchy-plugin-permission-contract
  omarchy-plugin-wire)
set_target_properties(omarchy-plugin-host-session PROPERTIES
  POSITION_INDEPENDENT_CODE ON)

if(BUILD_TESTING)
  find_package(Threads REQUIRED)
  omarchy_plugin_test(plugin-sqlite-authority-database
    COMMAND omarchy-plugin-sqlite-authority-database-test
    TIMEOUT 20 LABELS "unit;security;authority;adversarial;crash"
    SOURCES tests/sqlite_authority_database_test.cpp
    LIBRARIES omarchy-plugin-authority-database)
  omarchy_plugin_test(plugin-authority-sql
    COMMAND omarchy-plugin-authority-sql-test
    TIMEOUT 20 LABELS "unit;security;authority;adversarial;crash"
    SOURCES tests/authority_sql_test.cpp
    LIBRARIES omarchy-plugin-activation-snapshot)
  omarchy_plugin_test(plugin-audit-store COMMAND omarchy-plugin-audit-store-test
    TIMEOUT 30 LABELS "unit;integration;adversarial;audit;security"
    SOURCES ../audit/tests/audit_store_test.cpp
    LIBRARIES omarchy-plugin-host-session)

  omarchy_plugin_test(plugin-gesture-eligibility
    COMMAND omarchy-plugin-gesture-eligibility-test
    TIMEOUT 10 LABELS "unit;security;gesture;adversarial"
    SOURCES tests/gesture_eligibility_test.cpp
    LIBRARIES omarchy-plugin-host-session Threads::Threads)

  omarchy_plugin_test(plugin-activation-snapshot COMMAND omarchy-plugin-activation-snapshot-test
    TIMEOUT 15 LABELS "unit;security;adversarial"
    SOURCES tests/activation_snapshot_test.cpp
    LIBRARIES omarchy-plugin-activation-snapshot)

  omarchy_plugin_test(plugin-authority-store COMMAND omarchy-plugin-authority-store-test
    TIMEOUT 20 LABELS "unit;security;authority;adversarial"
    SOURCES tests/authority_store_test.cpp
    LIBRARIES omarchy-plugin-activation-snapshot)
  target_compile_definitions(omarchy-plugin-authority-store-test PRIVATE
    OMARCHY_AUTHORITY_STORE_TESTING)
  omarchy_plugin_test(plugin-authority-store-crash-matrix
    COMMAND omarchy-plugin-authority-store-test --crash-matrix-only
    TIMEOUT 20 LABELS "unit;security;authority;adversarial;crash")

  omarchy_plugin_test(plugin-consent-review COMMAND omarchy-plugin-consent-review-test
    TIMEOUT 20 LABELS "unit;security;authority;consent;adversarial"
    SOURCES tests/consent_review_test.cpp
    LIBRARIES omarchy-plugin-activation-snapshot)

  omarchy_plugin_test(plugin-multi-surface-router
    COMMAND omarchy-plugin-multi-surface-router-test
    TIMEOUT 15 LABELS "unit;security;session"
    SOURCES tests/MultiSurfaceRouter_test.cpp
    LIBRARIES omarchy-plugin-host-session)

  omarchy_plugin_test(plugin-structured-broker
    COMMAND omarchy-plugin-structured-broker-test
    TIMEOUT 30 LABELS "unit;security;session;broker;adversarial"
    SOURCES tests/structured_broker_test.cpp
      ../contracts/capability-definitions/tests/dynamic_activation_test.cpp
    LIBRARIES omarchy-plugin-host-session omarchy-plugin-providers)

  omarchy_plugin_test(plugin-gesture-intent COMMAND omarchy-plugin-gesture-intent-test
    TIMEOUT 10 LABELS "unit;security;session;gesture;adversarial"
    SOURCES tests/gesture_intent_test.cpp
    LIBRARIES omarchy-plugin-host-session)
endif()
