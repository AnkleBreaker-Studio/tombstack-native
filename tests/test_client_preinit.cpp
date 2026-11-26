#include "client_harness.h"
#include "fake_transport.h"
#include "test_framework.h"

#include <chrono>
#include <thread>

namespace fs = std::filesystem;
using client_harness::count_bodies;
using client_harness::count_files;
using client_harness::SdkSession;
using client_harness::write_file;

namespace {

constexpr const char *kToken = "tmb_test_preinit";

/** The state a crashed earlier session leaves: a pending crash report and its live marker. */
void plant_crashed_session(const fs::path &dir) {
    write_file(dir / "pending" / "crashes" / "1700000000000-cccccccc.json",
               R"({"occurredAtIso":"2026-11-01T00:00:00.000Z","buildVersion":"1.0.0",)"
               R"("os":"windows","arch":"x64","signature":"sig-earlier","stackHint":"h",)"
               R"("log":false})");
    write_file(dir / "session.lock",
               R"({"sessionId":"earlier","startedAtIso":"2026-11-01T00:00:00.000Z",)"
               R"("buildVersion":"1.0.0","os":"windows","arch":"x64"})");
}

}  // namespace

TEST_CASE("client_preinit", "a consent refusal made before init holds everything from the start") {
    ttest::TempDir dir;
    plant_crashed_session(dir.path());
    fake_transport::reset();
    CHECK(tombstone_set_consent(0) == TOMBSTONE_OK);  // the player's stored refusal
    {
        const SdkSession session{kToken, dir.path().string()};  // options keep consent_granted = 1
        CHECK(session.ok());
        CHECK(tombstone_flush(1000) == TOMBSTONE_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds{300});  // a heartbeat would be out by now
        CHECK(fake_transport::requests().empty());
        CHECK(!fs::exists(dir.path() / "session.lock"));  // no marker for this session
        CHECK_EQ(count_files(dir.path() / "pending" / "crashes"), std::size_t{1});  // kept

        CHECK(tombstone_set_consent(1) == TOMBSTONE_OK);  // the player accepts later on
        CHECK(tombstone_flush(5000) == TOMBSTONE_OK);
    }
    CHECK_EQ(count_bodies("/ingest/crashes", "sig-earlier"), std::size_t{1});
}

TEST_CASE("client_preinit", "identity and environment set before init ride the first heartbeat") {
    ttest::TempDir dir;
    fake_transport::reset();
    CHECK(tombstone_set_user("player-preinit", nullptr) == TOMBSTONE_OK);
    CHECK(tombstone_set_environment("staging") == TOMBSTONE_OK);
    const SdkSession session{kToken, dir.path().string()};
    CHECK(session.ok());
    CHECK(fake_transport::wait_for_requests(1, std::chrono::seconds{5}));
    const auto sent = fake_transport::requests();
    CHECK(sent.front().url.find("/ingest/heartbeats") != std::string::npos);
    CHECK(sent.front().body.find(R"("userId":"player-preinit")") != std::string::npos);
    CHECK(sent.front().body.find(R"("environment":"staging")") != std::string::npos);
}
