#include "client_harness.h"
#include "fake_transport.h"
#include "test_framework.h"

#include <string>

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
