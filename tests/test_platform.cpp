#include "platform.h"
#include "test_framework.h"

#include <string>

TEST_CASE("platform", "os_name is a value of the server's OS enum, matching this build") {
    const std::string os = tombstone::os_name();
    CHECK(os == "windows" || os == "macos" || os == "linux" || os == "android" || os == "ios" ||
          os == "other");
#if defined(_WIN32)
    CHECK_EQ(os, std::string{"windows"});
#elif defined(__ANDROID__)
    CHECK_EQ(os, std::string{"android"});  // not "linux", although Android defines __linux__
#elif defined(__linux__)
    CHECK_EQ(os, std::string{"linux"});
#endif
}

TEST_CASE("platform", "a default data root exists except where the platform has none") {
#if defined(__ANDROID__)
    CHECK(tombstone::default_data_root().empty());  // the game must pass options.data_dir
#else
    CHECK(!tombstone::default_data_root().empty());
#endif
}
