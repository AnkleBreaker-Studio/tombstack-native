#include "client_harness.h"
#include "fake_transport.h"
#include "sdk_log.h"
#include "session_log.h"
#include "sidecar_queue.h"
#include "test_framework.h"
#include "transport.h"
#include "worker.h"

#include <chrono>

namespace fs = std::filesystem;
using client_harness::count_files;
using client_harness::SdkSession;

namespace {

fake_transport::Response black_hole() {
    fake_transport::Response response;
    response.hang_until_abort = true;
    return response;
}

}  // namespace

TEST_CASE("client_shutdown", "stopping the worker cancels a stuck request and keeps its payload") {
    ttest::TempDir dir;
    fake_transport::reset();
    fake_transport::set_default(black_hole());
    tombstone::SdkLog log;
    tombstone::SessionLog session{log};
    tombstone::SidecarQueue sidecars{log};
    sidecars.configure(dir.path());
    session.configure(dir.path());
    tombstone::Transport transport{log};
    tombstone::Worker worker{transport, sidecars, session, log, "tmb_test_shutdown"};

    tombstone::UploadJob job;
    job.url = "http://fake.invalid/api/v1/ingest/bug-reports";
    job.body = R"({"message":"stuck on the wire","log":false})";
    job.durability = tombstone::Durability::persist_on_failure;
    job.kind = tombstone::SidecarKind::bug_report;
    job.sign_body = true;
    worker.enqueue(std::move(job));
    worker.start();
    CHECK(fake_transport::wait_for_requests(1, std::chrono::seconds{5}));  // now on the wire

    const auto started = std::chrono::steady_clock::now();
    worker.stop();
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds{1});
    CHECK_EQ(count_files(dir.path() / "pending" / "bug-reports"), std::size_t{1});  // next launch
}

TEST_CASE("client_shutdown", "quitting with the network black-holed takes about the quit window") {
    ttest::TempDir dir;
    fake_transport::reset();
    fake_transport::set_default(black_hole());
    const SdkSession session{"tmb_test_shutdown", dir.path().string()};
    CHECK(session.ok());
    CHECK(fake_transport::wait_for_requests(1, std::chrono::seconds{5}));  // the first heartbeat hangs
    CHECK(tombstone_report_crash("sig-quit", "crash just before quitting", nullptr, 0) ==
          TOMBSTONE_OK);

    const auto started = std::chrono::steady_clock::now();
    CHECK(tombstone_shutdown() == TOMBSTONE_OK);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::milliseconds{3500});  // the 2 s quit flush, then the abort
    CHECK_EQ(count_files(dir.path() / "pending" / "crashes"), std::size_t{1});  // kept for later
}
