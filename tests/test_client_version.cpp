#include <tombstone/tombstone.h>

#include "test_framework.h"

#include <string>

// tombstone_version() returns a literal in src/tombstone_api.cpp that must move with the
// CMake project version (0.9.1 shipped reporting 0.8.0). CMakeLists.txt passes the latter.

TEST_CASE("client_version", "tombstone_version matches the CMake project version") {
    CHECK_EQ(std::string{tombstone_version()}, std::string{TOMBSTONE_PROJECT_VERSION});
}
