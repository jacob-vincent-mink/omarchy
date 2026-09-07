cmake_minimum_required(VERSION 3.24)

project(omarchy_plugin_runtime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/common/include")
add_compile_options(-Wall -Wextra -Wpedantic -Werror)
option(OMARCHY_PLUGIN_SANITIZERS "Instrument runtime and tests with ASan/UBSan" OFF)
if(OMARCHY_PLUGIN_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

include(CTest)
include(GNUInstallDirs)
find_package(nlohmann_json REQUIRED)

# Keep each test command and its execution policy together at the call site.
function(omarchy_plugin_test name)
  cmake_parse_arguments(PARSE_ARGV 1 test "DEFAULTS"
    "TIMEOUT;LABELS;SKIP_RETURN_CODE;ENVIRONMENT" "COMMAND;SOURCES;LIBRARIES")
  if(test_UNPARSED_ARGUMENTS OR test_KEYWORDS_MISSING_VALUES OR NOT test_COMMAND
      OR ((test_LIBRARIES OR test_DEFAULTS) AND NOT test_SOURCES))
    message(FATAL_ERROR "Invalid test registration: ${name}")
  endif()
  if(test_SOURCES)
    list(GET test_COMMAND 0 target)
    add_executable(${target} ${test_SOURCES})
    if(test_DEFAULTS)
      omarchy_plugin_target_defaults(${target})
    endif()
    if(test_LIBRARIES)
      target_link_libraries(${target} PRIVATE ${test_LIBRARIES})
    endif()
  endif()
  add_test(NAME ${name} COMMAND ${test_COMMAND})
  foreach(property IN ITEMS TIMEOUT LABELS SKIP_RETURN_CODE ENVIRONMENT)
    if(DEFINED test_${property})
      set_tests_properties(${name} PROPERTIES ${property} "${test_${property}}")
    endif()
  endforeach()
endfunction()

set(OMARCHY_PLUGIN_QT_MIN_VERSION "" CACHE STRING "Optional minimum Qt 6 version required by the package build")
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install path prefix" FORCE)
endif()
if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
  message(FATAL_ERROR
    "The secure plugin runtime must be built for the canonical /usr prefix; use DESTDIR to stage it")
endif()
if(NOT CMAKE_INSTALL_LIBDIR STREQUAL "lib")
  message(FATAL_ERROR
    "The secure plugin runtime must use the canonical /usr/lib package root")
endif()

set(OMARCHY_PLUGIN_INSTALL_ROOT
  "lib/omarchy/plugin-security/${PROJECT_VERSION}")

set(OMARCHY_PLUGIN_PACKAGED_WORKER
  "/usr/${OMARCHY_PLUGIN_INSTALL_ROOT}/bin/omarchy-plugin-qml-worker")
install(CODE [[
if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
  message(FATAL_ERROR
      "The secure plugin runtime must be installed with its canonical /usr prefix; use DESTDIR to stage it")
endif()
]])
configure_file(cmake/runtime_paths.hpp.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/omarchy/plugin_runtime/runtime_paths.hpp"
  @ONLY)
add_library(omarchy-plugin-runtime-paths INTERFACE)
add_library(Omarchy::PluginRuntimePaths ALIAS omarchy-plugin-runtime-paths)
target_include_directories(omarchy-plugin-runtime-paths INTERFACE
  "${CMAKE_CURRENT_BINARY_DIR}/generated")

if(OMARCHY_PLUGIN_QT_MIN_VERSION)
  find_package(Qt6 ${OMARCHY_PLUGIN_QT_MIN_VERSION} REQUIRED COMPONENTS Core DBus Gui Qml Quick)
  qt_standard_project_setup(REQUIRES ${OMARCHY_PLUGIN_QT_MIN_VERSION})
else()
  find_package(Qt6 REQUIRED COMPONENTS Core DBus Gui Qml Quick)
  qt_standard_project_setup()
endif()

qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(QT_QML_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/qml")

function(omarchy_plugin_target_defaults target)
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endfunction()

add_subdirectory(contracts/manifest)
add_subdirectory(contracts/wire)
add_subdirectory(contracts/surface)
add_subdirectory(contracts/permissions)
add_subdirectory(contracts/capability-definitions)
add_subdirectory(broker)
add_subdirectory(providers)
add_subdirectory(discovery)
add_subdirectory(host-session)

if(BUILD_TESTING)
  add_subdirectory(tests/support)
endif()
