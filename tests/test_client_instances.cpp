#include "client.h"
#include "client_harness.h"
#include "fake_transport.h"
#include "test_framework.h"

#include <memory>
#include <string>

namespace fs = std::filesystem;
using client_harness::count_bodies;
using client_harness::write_file;

// Two Client instances on one data directory stand in for two processes of the
// same game (a second instance, a launcher relaunch, a client and a server).

TEST_CASE("client_instances", "a second process of the game leaves the first one's session alone") {
    ttest::TempDir dir;
    write_file(dir.path() / "pending" / "crashes" / "1700000000000-dddddddd.json",
               R"({"occurredAtIso":"2026-11-01T00:00:00.000Z","buildVersion":"1.0.0",)"
               R"("os":"windows","arch":"x64","signature":"sig-pending","stackHint":"h",)"
               R"("log":false})");
    fake_transport::reset();
    const std::string data_dir = dir.path().string();
    tombstone_options options;
    tombstone_options_init(&options);
    options.endpoint = client_harness::kEndpoint;
    options.token = "tmb_test_instances";
    options.build_version = "1.0.0-test";
    options.data_dir = data_dir.c_str();

    auto first = std::make_unique<tombstone::Client>();
    CHECK(first->init(options) == TOMBSTONE_OK);
    first->start();
    CHECK(first->flush(5000) == TOMBSTONE_OK);
    CHECK(fs::exists(dir.path() / "session.lock"));  // the first process's live session
    {
        tombstone::Client second;
        CHECK(second.init(options) == TOMBSTONE_OK);
        second.start();
        CHECK(second.report_crash("sig-second", "from the second process", nullptr, false) ==
              TOMBSTONE_OK);
        CHECK(second.flush(5000) == TOMBSTONE_OK);
    }  // the second process quits cleanly

    CHECK(fs::exists(dir.path() / "session.lock"));  // ...without deleting the first one's marker
    CHECK_EQ(count_bodies("/ingest/crashes", "unclean-shutdown"), std::size_t{0});
    CHECK_EQ(count_bodies("/ingest/crashes", "sig-pending"), std::size_t{1});  // sent once
    CHECK_EQ(count_bodies("/ingest/crashes", "sig-second"), std::size_t{1});   // its own report
    first.reset();
    CHECK(!fs::exists(dir.path() / "session.lock"));  // the owner's clean exit clears it
}
