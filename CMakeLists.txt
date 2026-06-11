cmake_minimum_required(VERSION 3.16)
project(tombstone VERSION 0.3.0 LANGUAGES C CXX)

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
find_package(CURL QUIET)
if(NOT CURL_FOUND)
  message(STATUS "tombstone: system libcurl not found; fetching curl 8.11.1")
  include(FetchContent)
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
  elseif(APPLE)
    set(CURL_USE_SECTRANSP ON CACHE BOOL "" FORCE)
  else()
    set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
  endif()
  FetchContent_Declare(
    curl
    URL https://github.com/curl/curl/releases/download/curl-8_11_1/curl-8.11.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
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
  src/json_scan.cpp
  src/json_writer.cpp
  src/payloads.cpp
  src/platform.cpp
  src/sdk_log.cpp
  src/session_log.cpp
  src/session_marker.cpp
  src/sidecar_queue.cpp
  src/signature.cpp
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
    tests/test_dedupe.cpp
    tests/test_json_scan.cpp
    tests/test_json_writer.cpp
    tests/test_payloads.cpp
    tests/test_session_log.cpp
    tests/test_session_marker.cpp
    tests/test_sidecar.cpp
    tests/test_signature.cpp
    $<TARGET_OBJECTS:tombstone_core>
  )
  target_include_directories(tombstone_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_link_libraries(tombstone_tests PRIVATE Threads::Threads tombstone_warnings)

  foreach(suite
      batch breadcrumb_ring clock dedupe json_scan json_writer
      payloads session_log session_marker sidecar signature)
    add_test(NAME ${suite} COMMAND tombstone_tests ${suite})
  endforeach()
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
