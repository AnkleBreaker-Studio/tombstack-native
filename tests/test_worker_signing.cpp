#include "fake_transport.h"
#include "test_framework.h"
#include "worker_rig.h"

#include <chrono>
#include <cstdlib>
#include <ctime>

namespace fs = std::filesystem;
using worker_rig::WorkerRig;

namespace {

std::int64_t device_now() { return static_cast<std::int64_t>(std::time(nullptr)); }

bool close_to(std::int64_t actual, std::int64_t expected) { return std::llabs(actual - expected) <= 5; }

fake_transport::Response unauthorized(const char *code) {
    fake_transport::Response response;
    response.status = 401;
    response.body = std::string{R"({"success":false,"error":"unauthorized","code":")"} + code +
                    R"("})";
    return response;
}

}  // namespace

TEST_CASE("worker_signing", "a signature 401 corrects the clock from Date and retries at once") {
    ttest::TempDir dir;
    WorkerRig rig{dir.path()};
    const std::int64_t now = device_now();
    fake_transport::Response refused = unauthorized("invalid_signature");
    refused.date = fake_transport::http_date(now + 600);  // the server is 10 minutes ahead
    fake_transport::push("/ingest/crashes", refused);

    const fs::path sidecar = rig.enqueue_crash();
    const auto started = std::chrono::steady_clock::now();
    rig.worker->start();
    CHECK(rig.worker->flush(std::chrono::seconds{10}));
    const auto elapsed = std::chrono::steady_clock::now() - started;

    const auto sent = fake_transport::requests();
    CHECK_EQ(sent.size(), std::size_t{2});
    CHECK(sent[0].signed_body && close_to(sent[0].sign_unix_seconds, now));        // device clock
    CHECK(sent[1].signed_body && close_to(sent[1].sign_unix_seconds, now + 600));  // server clock
    CHECK(elapsed < std::chrono::seconds{2});  // not held for the 2 s backoff
    CHECK(!fs::exists(sidecar));               // delivered by the retry
}

TEST_CASE("worker_signing", "a signature 401 never deletes the report, even without a Date") {
    ttest::TempDir dir;
    WorkerRig rig{dir.path()};
    fake_transport::set_default(unauthorized("invalid_signature"));  // no Date: nothing to learn

    const fs::path sidecar = rig.enqueue_crash();
    rig.worker->start();
    CHECK(fake_transport::wait_for_requests(1, std::chrono::seconds{5}));
    rig.worker->stop();  // the retry is waiting out its backoff; the report must survive
    CHECK(fs::exists(sidecar));
}

TEST_CASE("worker_signing", "a refused SDK token is not mistaken for a clock problem") {
    // invalid_api_key is not retried: it stops sending for the launch and keeps the report
    // (worker_status); only the signature refusal is a clock problem.
    ttest::TempDir dir;
    WorkerRig rig{dir.path()};
    fake_transport::Response refused = unauthorized("invalid_api_key");
    refused.date = fake_transport::http_date(device_now() + 600);
    fake_transport::push("/ingest/crashes", refused);

    const fs::path sidecar = rig.enqueue_crash();
    rig.worker->start();
    CHECK(rig.worker->flush(std::chrono::seconds{5}));
    CHECK_EQ(fake_transport::requests().size(), std::size_t{1});
    CHECK(fs::exists(sidecar));
}

TEST_CASE("worker_signing", "a validation 400 still drops the payload") {
    ttest::TempDir dir;
    WorkerRig rig{dir.path()};
    fake_transport::Response invalid;
    invalid.status = 400;
    invalid.body = R"({"success":false,"error":"validation failed"})";
    fake_transport::push("/ingest/crashes", invalid);

    const fs::path sidecar = rig.enqueue_crash();
    rig.worker->start();
    CHECK(rig.worker->flush(std::chrono::seconds{5}));
    CHECK_EQ(fake_transport::requests().size(), std::size_t{1});
    CHECK(!fs::exists(sidecar));
}

TEST_CASE("worker_signing", "every ingest response teaches the clock, success included") {
    ttest::TempDir dir;
    WorkerRig rig{dir.path()};
    const std::int64_t now = device_now();
    fake_transport::Response accepted;
    accepted.date = fake_transport::http_date(now - 900);  // the device runs 15 minutes fast
    fake_transport::push("/ingest/crashes", accepted);

    rig.enqueue_crash();
    rig.enqueue_crash();
    rig.worker->start();
    CHECK(rig.worker->flush(std::chrono::seconds{5}));
    const auto sent = fake_transport::requests();
    CHECK_EQ(sent.size(), std::size_t{2});
    CHECK(close_to(sent[1].sign_unix_seconds, now - 900));
}
