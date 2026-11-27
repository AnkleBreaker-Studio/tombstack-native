#include "client_harness.h"
#include "fake_transport.h"
#include "test_framework.h"

namespace fs = std::filesystem;
using client_harness::count_bodies;
using client_harness::count_files;
using client_harness::SdkSession;

namespace {

constexpr const char *kToken = "tmb_test_consent";

fake_transport::Response offline() {
    fake_transport::Response response;
    response.transport_error = true;
    return response;
}

std::uintmax_t file_size_or_zero(const fs::path &path) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    return ec ? 0 : size;
}

}  // namespace

TEST_CASE("client_consent", "withdrawing consent discards everything not yet delivered") {
    ttest::TempDir dir;
    fake_transport::reset();
    fake_transport::set_default(offline());  // nothing gets through: everything stays queued
    const SdkSession session{kToken, dir.path().string()};
    CHECK(session.ok());
    CHECK(tombstone_log_line(TOMBSTONE_LEVEL_INFO, "line before the withdrawal") == TOMBSTONE_OK);
    CHECK(tombstone_report_crash("sig-before", "crash before", nullptr, 1) == TOMBSTONE_OK);
    CHECK(tombstone_report_bug(nullptr, "bug before the withdrawal", 0) == TOMBSTONE_OK);
    for (int i = 0; i < 10; ++i) {
        CHECK(tombstone_track_event("event-before", nullptr, nullptr, 0) == TOMBSTONE_OK);
    }
    CHECK_EQ(count_files(dir.path() / "pending" / "crashes"), std::size_t{1});
    CHECK_EQ(count_files(dir.path() / "pending" / "bug-reports"), std::size_t{1});
    CHECK(file_size_or_zero(dir.path() / "session.log") > 0);

    CHECK(tombstone_set_consent(0) == TOMBSTONE_OK);
    fake_transport::reset();  // the network is back: anything still queued would go out now
    CHECK(tombstone_flush(2000) == TOMBSTONE_OK);

    CHECK_EQ(count_bodies("/ingest/crashes", "sig-before"), std::size_t{0});
    CHECK_EQ(count_bodies("/ingest/bug-reports", "bug before"), std::size_t{0});
    CHECK_EQ(count_bodies("/ingest/events:batch", "event-before"), std::size_t{0});
    CHECK_EQ(count_files(dir.path() / "pending" / "crashes"), std::size_t{0});
    CHECK_EQ(count_files(dir.path() / "pending" / "bug-reports"), std::size_t{0});
    CHECK_EQ(file_size_or_zero(dir.path() / "session.log"), std::uintmax_t{0});
    CHECK(!fs::exists(dir.path() / "previous-session.log"));
    CHECK(!fs::exists(dir.path() / "session.lock"));
    CHECK(tombstone_report_crash("sig-while-off", "h", nullptr, 0) == TOMBSTONE_DISABLED);
}

TEST_CASE("client_consent", "nothing captured before a withdrawal is sent after a re-grant") {
    ttest::TempDir dir;
    fake_transport::reset();
    fake_transport::set_default(offline());
    const SdkSession session{kToken, dir.path().string()};
    CHECK(session.ok());
    CHECK(tombstone_report_crash("sig-before", "crash before", nullptr, 0) == TOMBSTONE_OK);
    CHECK(tombstone_track_event("event-before", nullptr, nullptr, 0) == TOMBSTONE_OK);

    CHECK(tombstone_set_consent(0) == TOMBSTONE_OK);
    CHECK(tombstone_set_consent(1) == TOMBSTONE_OK);
    fake_transport::reset();
    CHECK(tombstone_report_crash("sig-after", "crash after the re-grant", nullptr, 0) ==
          TOMBSTONE_OK);
    CHECK(tombstone_flush(5000) == TOMBSTONE_OK);

    CHECK_EQ(count_bodies("/ingest/crashes", "sig-before"), std::size_t{0});
    CHECK_EQ(count_bodies("/ingest/events:batch", "event-before"), std::size_t{0});
    CHECK_EQ(count_bodies("/ingest/crashes", "sig-after"), std::size_t{1});
}

TEST_CASE("client_consent", "with consent off at init, earlier reports wait unsent until granted") {
    ttest::TempDir dir;
    fake_transport::reset();
    fake_transport::set_default(offline());
    {
        const SdkSession earlier{kToken, dir.path().string()};
        CHECK(earlier.ok());
        CHECK(tombstone_report_crash("sig-earlier", "earlier session", nullptr, 0) ==
              TOMBSTONE_OK);
    }
    CHECK_EQ(count_files(dir.path() / "pending" / "crashes"), std::size_t{1});

    fake_transport::reset();
    const SdkSession later{kToken, dir.path().string(),
                           [](tombstone_options &options) { options.consent_granted = 0; }};
    CHECK(later.ok());
    CHECK(tombstone_flush(1000) == TOMBSTONE_OK);
    CHECK_EQ(count_bodies("/ingest/crashes", "sig-earlier"), std::size_t{0});
    CHECK_EQ(count_files(dir.path() / "pending" / "crashes"), std::size_t{1});  // kept, not lost

    CHECK(tombstone_set_consent(1) == TOMBSTONE_OK);
    CHECK(tombstone_flush(5000) == TOMBSTONE_OK);
    CHECK_EQ(count_bodies("/ingest/crashes", "sig-earlier"), std::size_t{1});
}

TEST_CASE("client_consent", "the device identity is read and stored only once consent is granted") {
    ttest::TempDir dir;
    fake_transport::reset();
    const fs::path identity = dir.path() / "identity";
    const SdkSession session{kToken, dir.path().string(),
                             [](tombstone_options &options) { options.consent_granted = 0; }};
    CHECK(session.ok());
    CHECK(!fs::exists(identity));  // nothing stored on the device while consent is pending

    CHECK(tombstone_set_consent(1) == TOMBSTONE_OK);
    CHECK(fs::exists(identity));
    CHECK(tombstone_report_crash("sig-identity", "h", nullptr, 0) == TOMBSTONE_OK);
    CHECK(tombstone_flush(5000) == TOMBSTONE_OK);
    CHECK_EQ(count_bodies("/ingest/crashes", R"("userId":"dev_)"), std::size_t{1});  // never anonymous

    CHECK(tombstone_set_consent(0) == TOMBSTONE_OK);
    CHECK(!fs::exists(identity));  // withdrawn: the stored identifier is deleted
}

TEST_CASE("client_consent", "an operator log pull is not player data and is not held") {
    ttest::TempDir dir;
    fake_transport::reset();
    const SdkSession session{kToken, dir.path().string()};
    CHECK(session.ok());
    CHECK(tombstone_set_consent(0) == TOMBSTONE_OK);
    tombstone_request_player_logs(nullptr, "userId", "player-1", "support ticket");
    CHECK(tombstone_flush(5000) == TOMBSTONE_OK);
    CHECK_EQ(fake_transport::count_suffix("/api/v1/pull-requests"), std::size_t{1});
}
