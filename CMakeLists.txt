cmake_minimum_required(VERSION 3.18)
# NOTE: this version is DUPLICATED as a literal in src/tombstone_api.cpp (`sdk_version`), which is what
# tombstone_version() returns. Bump both together — 0.9.1's changelog records that the reported version
# was once stuck at 0.8.0 for exactly this reason. Deriving it from PROJECT_VERSION is the real fix.
project(tombstone VERSION 0.9.4 LANGUAGES C CXX)

option(TOMBSTONE_BUILD_STATIC "Also build a static tombstone library" OFF)
option(TOMBSTONE_BUILD_EXAMPLES "Build the example programs" ON)
option(TOMBSTONE_BUILD_TESTS "Build and register the unit tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Warnings-as-errors for OUR targets only (never for fetched dependencies).
# ---------------------------------------------------------------------------
add_library(tombstone_warnings INTERFACE)
if(MSVC)
  target_compile_options(tombstone_warnings INTERFACE /W4 /WX /permissive- /utf-8)
  target_compile_definitions(tombstone_warnings INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  target_compile_options(tombstone_warnings INTERFACE -Wall -Wextra -Werror)
endif()

# ---------------------------------------------------------------------------
# libcurl: system package first, FetchContent fallback (HTTP+TLS only).
# ---------------------------------------------------------------------------
find_package(CURL 7.56 QUIET)
if(NOT CURL_FOUND)
  message(STATUS "tombstone: system libcurl not found; fetching curl 8.22.0")
  include(FetchContent)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
  set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
  set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
  set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(HTTP_ONLY ON CACHE BOOL "" FORCE)
  set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
  set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
  set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
  set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
  set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
  set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
  if(WIN32)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
  else()
    set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
  endif()
  FetchContent_Declare(
    curl
    URL https://curl.se/download/curl-8.22.0.tar.gz
    URL_HASH SHA256=d54dd598bf05927a726deb38df31c6a255ba83ff1de57c5d1464dac3ed8f44a1
  )
  FetchContent_MakeAvailable(curl)
endif()

# ---------------------------------------------------------------------------
# Core object library: the PURE parts (no curl, no threads-of-its-own).
# Shared by the SDK and the unit tests, so tests never need a TLS stack.
# ---------------------------------------------------------------------------
set(TOMBSTONE_CORE_SOURCES
  src/batch.cpp
  src/breadcrumb_ring.cpp
  src/clock.cpp
  src/dedupe.cpp
  src/delivery_policy.cpp
  src/device_identity.cpp
  src/frame_stats.cpp
  src/instance_lock.cpp
  src/json_scan.cpp
  src/json_writer.cpp
  src/native_crash.cpp
  src/payloads.cpp
  src/platform.cpp
  src/preinit.cpp
  src/retry_after.cpp
  src/sampler.cpp
  src/sdk_log.cpp
  src/server_clock.cpp
  src/session_log.cpp
  src/session_marker.cpp
  src/sidecar_queue.cpp
  src/signature.cpp
  src/storage_owner.cpp
)

add_library(tombstone_core OBJECT ${TOMBSTONE_CORE_SOURCES})
target_include_directories(tombstone_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(tombstone_core PRIVATE tombstone_warnings)

set(TOMBSTONE_IMPURE_SOURCES
  src/client.cpp
  src/client_reports.cpp
  src/tombstone_api.cpp
  src/transport_curl.cpp
  src/worker.cpp
)

# ---------------------------------------------------------------------------
# The shared library (the artifact engines load).
# ---------------------------------------------------------------------------
add_library(tombstone SHARED ${TOMBSTONE_IMPURE_SOURCES} $<TARGET_OBJECTS:tombstone_core>)
target_include_directories(tombstone
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(tombstone PRIVATE TOMBSTONE_EXPORTS)
target_link_libraries(tombstone PRIVATE CURL::libcurl Threads::Threads tombstone_warnings)
set_target_properties(tombstone PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

# Optional static variant. Consumers must define TOMBSTONE_STATIC, which the
# INTERFACE definition propagates automatically.
if(TOMBSTONE_BUILD_STATIC)
  add_library(tombstone_static STATIC ${TOMBSTONE_IMPURE_SOURCES} $<TARGET_OBJECTS:tombstone_core>)
  target_include_directories(tombstone_static
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(tombstone_static PUBLIC TOMBSTONE_STATIC)
  target_link_libraries(tombstone_static PRIVATE CURL::libcurl Threads::Threads tombstone_warnings)
endif()

# ---------------------------------------------------------------------------
# Examples (pure C99 consumers of the shared library).
# ---------------------------------------------------------------------------
if(TOMBSTONE_BUILD_EXAMPLES)
  add_executable(tombstone_minimal examples/minimal.c)
  target_link_libraries(tombstone_minimal PRIVATE tombstone)

  add_executable(tombstone_crash_sidecar_demo examples/crash_sidecar_demo.c)
  target_link_libraries(tombstone_crash_sidecar_demo PRIVATE tombstone)
endif()

# ---------------------------------------------------------------------------
# Tests: dependency-free runner over the pure core; one CTest entry per suite.
# ---------------------------------------------------------------------------
if(TOMBSTONE_BUILD_TESTS)
  enable_testing()
  add_executable(tombstone_tests
    tests/main.cpp
    tests/test_batch.cpp
    tests/test_breadcrumb_ring.cpp
    tests/test_clock.cpp
    tests/test_concurrency.cpp
    tests/test_dedupe.cpp
    tests/test_delivery_policy.cpp
    tests/test_device_identity.cpp
    tests/test_frame_stats.cpp
    tests/test_header_contract.cpp
    tests/test_instance_lock.cpp
    tests/test_json_scan.cpp
    tests/test_json_writer.cpp
    tests/test_native_crash.cpp
    tests/test_payloads.cpp
    tests/test_platform.cpp
    tests/test_preinit.cpp
    tests/test_retry_after.cpp
    tests/test_sampler.cpp
    tests/test_server_clock.cpp
    tests/test_session_log.cpp
    tests/test_session_marker.cpp
    tests/test_sidecar.cpp
    tests/test_signature.cpp
    tests/test_start_gate.cpp
    $<TARGET_OBJECTS:tombstone_core>
  )
  target_include_directories(tombstone_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_link_libraries(tombstone_tests PRIVATE Threads::Threads tombstone_warnings)
  # test_header_contract.cpp reads the public header from the source tree.
  target_compile_definitions(tombstone_tests PRIVATE
    "TOMBSTONE_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")

  foreach(suite
      batch breadcrumb_ring clock concurrency dedupe delivery_policy device_identity frame_stats header_contract
      instance_lock json_scan json_writer native_crash payloads platform preinit retry_after sampler server_clock
      session_log session_marker sidecar signature start_gate)
    add_test(NAME ${suite} COMMAND tombstone_tests ${suite})
  endforeach()

  # Delivery, client and C ABI behaviour over an in-process fake transport (tests/fake_transport.cpp
  # replaces src/transport_curl.cpp at link time): no network and no libcurl needed.
  set(TOMBSTONE_CLIENT_TEST_SDK_SOURCES ${TOMBSTONE_IMPURE_SOURCES})
  list(REMOVE_ITEM TOMBSTONE_CLIENT_TEST_SDK_SOURCES src/transport_curl.cpp)
  add_executable(tombstone_client_tests
    tests/main.cpp
    tests/fake_transport.cpp
    tests/test_client_consent.cpp
    tests/test_client_instances.cpp
    tests/test_client_native_report.cpp
    tests/test_client_preinit.cpp
    tests/test_client_shutdown.cpp
    tests/test_client_storage.cpp
    tests/test_client_telemetry.cpp
    tests/test_worker_signing.cpp
    tests/test_worker_status.cpp
    ${TOMBSTONE_CLIENT_TEST_SDK_SOURCES}
    $<TARGET_OBJECTS:tombstone_core>
  )
  target_compile_definitions(tombstone_client_tests PRIVATE TOMBSTONE_STATIC)
  target_include_directories(tombstone_client_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_link_libraries(tombstone_client_tests PRIVATE Threads::Threads tombstone_warnings)

  foreach(suite client_consent client_instances client_native_report client_preinit
      client_shutdown client_storage client_telemetry worker_signing worker_status)
    add_test(NAME ${suite} COMMAND tombstone_client_tests ${suite})
  endforeach()

  find_program(TOMBSTONE_NODE_EXECUTABLE node)
  if(TOMBSTONE_NODE_EXECUTABLE)
    add_executable(tombstone_wire_probe tests/wire_probe.cpp src/worker.cpp src/transport_curl.cpp
      $<TARGET_OBJECTS:tombstone_core>)
    target_include_directories(tombstone_wire_probe PRIVATE src include)
    target_link_libraries(tombstone_wire_probe PRIVATE CURL::libcurl Threads::Threads tombstone_warnings)
    add_test(NAME transport_wire COMMAND ${TOMBSTONE_NODE_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/wire-test.mjs $<TARGET_FILE:tombstone_wire_probe>)
    set_tests_properties(transport_wire PROPERTIES TIMEOUT 60)
  endif()
endif()

# ---------------------------------------------------------------------------
# Install rules.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS tombstone
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
if(TOMBSTONE_BUILD_STATIC)
  install(TARGETS tombstone_static ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(DIRECTORY include/tombstone DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
