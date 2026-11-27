#include "client_harness.h"
#include "fake_transport.h"
#include "json_scan.h"
#include "test_framework.h"

#include <string>

using client_harness::SdkSession;
using client_harness::write_file;

namespace {

/** The crash report sent at the launch after a native crash at `offset` in libgame.so. */
std::string report_after_native_crash(const char *offset) {
    ttest::TempDir dir;
    write_file(dir.path() / "native-crash.dump",
               std::string{"tombstone-native-crash 1\nsignal 11\ncode 1\nfault 0x0\n"
                           "frame libgame.so "} +
                   offset + " aabbccdd\nend\n");
    write_file(dir.path() / "session.lock",
               R"({"sessionId":"crashed","startedAtIso":"2026-11-01T00:00:00.000Z",)"
               R"("buildVersion":"1.0.0","os":"linux","arch":"x64"})");
    fake_transport::reset();
    {
        const SdkSession session{"tmb_test_native", dir.path().string(),
                                 [](tombstone_options &options) {
                                     options.enable_native_crash_handler = 1;
                                 }};
        CHECK(session.ok());
        CHECK(tombstone_flush(5000) == TOMBSTONE_OK);
    }
    for (const fake_transport::Request &request : fake_transport::requests()) {
        if (request.url.find("/ingest/crashes") != std::string::npos) {
            return request.body;
        }
    }
    return {};
}

}  // namespace

TEST_CASE("client_native_report", "native crashes at different code sites keep apart") {
    const std::string first = report_after_native_crash("0x10");
    const std::string second = report_after_native_crash("0x20");
    CHECK(first.find(R"("crashType":"native_crash")") != std::string::npos);
    const std::string first_key = tombstone::find_string_field(first, "fingerprint").value_or("");
    const std::string second_key = tombstone::find_string_field(second, "fingerprint").value_or("");
    CHECK(!first_key.empty());
    CHECK(first_key != second_key);  // the server would merge these on their stacks alone
    CHECK_EQ(first_key, tombstone::find_string_field(first, "signature").value_or(""));
}
