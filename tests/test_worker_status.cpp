// How the worker acts on the server's refusals (delivery_policy.h): a 402 spending-cap
// stop, a refused SDK token (401 invalid_api_key / 403), and a payload the server
// refuses (404). Runs the real Worker over the fake transport.

#include "clock.h"
#include "fake_transport.h"
#include "test_framework.h"
#include "worker_rig.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>

namespace fs = std::filesystem;
using std::chrono::milliseconds;
using std::chrono::seconds;
using worker_rig::WorkerRig;

namespace {

std::int64_t device_now() { return static_cast<std::int64_t>(std::time(nullptr)); }

fake_transport::Response spend_cap(std::int64_t resumes_at_unix) {
    fake_transport::Response response;
    response.status = 402;
    response.body = R"({"success":false,"error":"spend_cap_reached","message":"Spending cap reached.",)"
                    R"("resumesAt":")" +
                    tombstone::format_iso8601_utc_ms(resumes_at_unix * 1000) + R"("})";
    return response;
}

fake_transport::Response refused(long status, const char *body) {
    fake_transport::Response response;
    response.status = status;
    response.body = body;
    return response;
}

/** True when no request beyond the first `count` is made within `window`. */
bool nothing_more_sent(std::size_t count, milliseconds window) {
    return !fake_transport::wait_for_requests(count + 1, window);
}

}  // namespace

TEST_CASE("worker_status", "a 402 keeps the report and sends nothing during the stop") {
    ttest::TempDir dir;
    WorkerRig rig{dir.path()};
    fake_transport::push("/ingest/crashes", spend_cap(device_now() + 3600));

    const fs::path first = rig.enqueue_crash();
    rig.worker->start();
    CHECK(fake_transport::wait_for_requests(1, seconds{5}));
    // During the stop: a heartbeat, a new events batch and another crash.
    rig.enqueue_heartbeat();
    rig.enqueue_events_batch();
    const fs::path second = rig.enqueue_crash();
    CHECK(rig.worker->flush(seconds{2}));  // parked reports do not hold a flush
    CHECK(nothing_more_sent(1, milliseconds{1500}));

    rig.worker->stop();
    CHECK(fs::exists(first));   // kept: sent once the stop ends, or at a later launch
    CHECK(fs::exists(second));
    CHECK_EQ(rig.files("events-batch"), std::size_t{0});  // a hard stop: not kept for later
}

TEST_CASE("worker_status", "a report parked by a 402 is sent once resumesAt is reached") {
    ttest::TempDir dir;
    WorkerRig rig{dir.path()};
    rig.worker->set_min_spend_cap_stop(seconds{1});
    fake_transport::push("/ingest/crashes", spend_cap(device_now() + 2));

    const fs::path report = rig.enqueue_crash();
    const auto started = std::chrono::steady_clock::now();
    rig.worker->start();
    CHECK(fake_transport::wait_for_requests(2, seconds{10}));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(rig.worker->flush(seconds{5}));

    CHECK(elapsed >= milliseconds{900});  // not before the stop ended
    CHECK_EQ(fake_transport::count_suffix("/ingest/crashes"), std::size_t{2});
    CHECK(!fs::exists(report));  // delivered after the stop
}

TEST_CASE("worker_status", "a refused token stops every request for the launch") {
    ttest::TempDir dir;
    WorkerRig rig{dir.path()};
    fake_transport::push("/ingest/crashes",
                         refused(401, R"({"success":false,"code":"invalid_api_key"})"));

    const fs::path first = rig.enqueue_crash();
    const fs::path second = rig.enqueue_crash();
    rig.worker->start();
    CHECK(fake_transport::wait_for_requests(1, seconds{5}));
    CHECK(rig.worker->flush(seconds{2}));
    rig.enqueue_heartbeat();
    rig.enqueue_crash_without_file();  // its sidecar could not be written before
    CHECK(nothing_more_sent(1, milliseconds{1500}));

    rig.worker->stop();
    CHECK(fs::exists(first));   // kept on disk for a later launch
    CHECK(fs::exists(second));
    CHECK_EQ(rig.files("crashes"), std::size_t{3});  // the third is written now, not lost
}

TEST_CASE("worker_status", "a 403 on an ingest POST refuses the token the same way") {
    ttest::TempDir dir;
    WorkerRig rig{dir.path()};
    fake_transport::push("/ingest/events:batch", refused(403, "<html>Forbidden</html>"));

    rig.enqueue_events_batch();
    const fs::path report = rig.enqueue_crash();
    rig.worker->start();
    CHECK(fake_transport::wait_for_requests(1, seconds{5}));
    CHECK(rig.worker->flush(seconds{2}));
    CHECK(nothing_more_sent(1, milliseconds{1500}));

    rig.worker->stop();
    CHECK(fs::exists(report));
    CHECK_EQ(rig.files("events-batch"), std::size_t{0});
}

TEST_CASE("worker_status", "a refused log-pull call stops nothing else") {
    // An ingest-only token may not fulfil every log pull: that 403 is not a refused token.
    ttest::TempDir dir;
    WorkerRig rig{dir.path()};
    fake_transport::push("/fulfill", refused(403, R"({"success":false,"error":"forbidden"})"));

    rig.enqueue_pull_request();
    const fs::path report = rig.enqueue_crash();
    rig.worker->start();
    CHECK(fake_transport::wait_for_requests(2, seconds{5}));
    CHECK(rig.worker->flush(seconds{5}));
    CHECK_EQ(fake_transport::count_suffix("/ingest/crashes"), std::size_t{1});
    CHECK(!fs::exists(report));
}

TEST_CASE("worker_status", "a 404 drops that report and stops nothing else") {
    ttest::TempDir dir;
    WorkerRig rig{dir.path()};
    fake_transport::push("/ingest/crashes", refused(404, R"({"success":false,"error":"not found"})"));

    const fs::path first = rig.enqueue_crash();
    const fs::path second = rig.enqueue_crash();
    rig.worker->start();
    CHECK(rig.worker->flush(seconds{5}));
    CHECK_EQ(fake_transport::count_suffix("/ingest/crashes"), std::size_t{2});
    CHECK(!fs::exists(first));   // refused itself: deleted
    CHECK(!fs::exists(second));  // delivered
}

TEST_CASE("worker_status", "a 402 on a log-pull call stops ingestion too") {
    // The spending cap refuses every route, the log-pull fulfilment included.
    ttest::TempDir dir;
    WorkerRig rig{dir.path()};
    fake_transport::push("/fulfill", spend_cap(device_now() + 3600));

    rig.enqueue_pull_request();
    rig.worker->start();
    CHECK(fake_transport::wait_for_requests(1, seconds{5}));
    CHECK(rig.worker->flush(seconds{2}));
    const fs::path report = rig.enqueue_crash();
    CHECK(nothing_more_sent(1, milliseconds{1500}));
    rig.worker->stop();
    CHECK(fs::exists(report));
}
