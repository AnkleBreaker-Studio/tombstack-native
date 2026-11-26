#include "client_harness.h"
#include "fake_transport.h"
#include "test_framework.h"

#include <string>

namespace fs = std::filesystem;
using client_harness::count_files;
using client_harness::SdkSession;

namespace {

/** Occurrences of `needle` across the bodies sent to URLs ending with `url_suffix`. */
std::size_t occurrences(const char *url_suffix, const std::string &needle) {
    std::size_t total = 0;
    const std::string suffix{url_suffix};
    for (const fake_transport::Request &request : fake_transport::requests()) {
        if (request.url.size() < suffix.size() ||
            request.url.compare(request.url.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }
        for (std::size_t at = request.body.find(needle); at != std::string::npos;
             at = request.body.find(needle, at + needle.size())) {
            ++total;
        }
    }
    return total;
}

}  // namespace

TEST_CASE("client_telemetry", "the round-trip self-metric is sampled, not one row per upload") {
    ttest::TempDir dir;
    fake_transport::reset();
    {
        const SdkSession session{"tmb_test_telemetry", dir.path().string()};
        CHECK(session.ok());
        // A heartbeat and three crash reports: four successful uploads in a few milliseconds.
        CHECK(tombstone_report_crash("sig-1", "first", nullptr, 0) == TOMBSTONE_OK);
        CHECK(tombstone_report_crash("sig-2", "second", nullptr, 0) == TOMBSTONE_OK);
        CHECK(tombstone_report_crash("sig-3", "third", nullptr, 0) == TOMBSTONE_OK);
        CHECK(tombstone_flush(5000) == TOMBSTONE_OK);  // all delivered: rtt samples recorded
        CHECK(tombstone_flush(5000) == TOMBSTONE_OK);  // ships the metrics batch
    }
    CHECK_EQ(fake_transport::count_suffix("/ingest/crashes"), std::size_t{3});
    CHECK_EQ(occurrences("/ingest/metrics:batch", "\"tombstone.rtt_ms\""), std::size_t{1});
}

TEST_CASE("client_telemetry", "events and metrics not sent before an offline quit go out next launch") {
    ttest::TempDir dir;
    fake_transport::reset();
    fake_transport::Response offline;
    offline.transport_error = true;
    fake_transport::set_default(offline);
    const auto no_rtt = [](tombstone_options &options) { options.enable_rtt_metric = 0; };
    {
        const SdkSession session{"tmb_test_telemetry", dir.path().string(), no_rtt};
        CHECK(session.ok());
        CHECK(tombstone_track_event("level-complete", nullptr, nullptr, 0) == TOMBSTONE_OK);
        tombstone_track_metric(nullptr, "frame-rate-test", 59.5, "fps");
        (void)tombstone_flush(200);  // the batches go out and fail
    }  // quitting offline
    CHECK_EQ(count_files(dir.path() / "pending" / "events-batch"), std::size_t{1});
    CHECK_EQ(count_files(dir.path() / "pending" / "metrics-batch"), std::size_t{1});

    fake_transport::reset();  // online again
    {
        const SdkSession session{"tmb_test_telemetry", dir.path().string(), no_rtt};
        CHECK(session.ok());
        CHECK(tombstone_flush(5000) == TOMBSTONE_OK);
    }
    CHECK_EQ(occurrences("/ingest/events:batch", "\"level-complete\""), std::size_t{1});
    CHECK_EQ(occurrences("/ingest/metrics:batch", "\"frame-rate-test\""), std::size_t{1});
    CHECK_EQ(count_files(dir.path() / "pending" / "events-batch"), std::size_t{0});
    CHECK_EQ(count_files(dir.path() / "pending" / "metrics-batch"), std::size_t{0});
}
