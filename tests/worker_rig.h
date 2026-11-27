#ifndef TOMBSTONE_TESTS_WORKER_RIG_H
#define TOMBSTONE_TESTS_WORKER_RIG_H

// One Worker over the fake transport (tests/fake_transport.cpp) and a real sidecar
// queue in a temp directory, for the worker-level suites of tombstone_client_tests.

#include "fake_transport.h"
#include "sdk_log.h"
#include "session_log.h"
#include "sidecar_queue.h"
#include "transport.h"
#include "worker.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace worker_rig {

inline constexpr const char *kCrashUrl = "http://fake.invalid/api/v1/ingest/crashes";
inline constexpr const char *kHeartbeatUrl = "http://fake.invalid/api/v1/ingest/heartbeats";
inline constexpr const char *kEventsBatchUrl = "http://fake.invalid/api/v1/ingest/events:batch";
inline constexpr const char *kPullRequestUrl =
    "http://fake.invalid/api/v1/pull-requests/req_1/fulfill";

struct WorkerRig {
    tombstone::SdkLog log;
    tombstone::SessionLog session{log};
    tombstone::SidecarQueue sidecars{log};
    tombstone::Transport transport{log};
    std::unique_ptr<tombstone::Worker> worker;
    std::filesystem::path pending;

    explicit WorkerRig(const std::filesystem::path &dir) : pending(dir / "pending") {
        fake_transport::reset();
        sidecars.configure(dir);
        session.configure(dir);
        worker = std::make_unique<tombstone::Worker>(transport, sidecars, session, log, "tmb_test");
    }
    ~WorkerRig() { worker->stop(); }
    WorkerRig(const WorkerRig &) = delete;
    WorkerRig &operator=(const WorkerRig &) = delete;

    /** Enqueue a write-ahead crash backed by a sidecar file; returns that file. */
    std::filesystem::path enqueue_crash() {
        tombstone::UploadJob job = crash_job();
        job.sidecar_path = sidecars.write(job.kind, job.body);
        std::filesystem::path file = job.sidecar_path;
        worker->enqueue(std::move(job));
        return file;
    }

    /** A write-ahead crash whose sidecar could not be written (no backing file). */
    void enqueue_crash_without_file() { worker->enqueue(crash_job()); }

    void enqueue_heartbeat() {
        tombstone::UploadJob job;
        job.url = kHeartbeatUrl;
        job.body = R"({"sessionId":"s"})";
        job.durability = tombstone::Durability::ephemeral;
        job.sign_body = true;
        worker->enqueue(std::move(job));
    }

    /** An events:batch envelope still in memory (persisted only on failure / at quit). */
    void enqueue_events_batch() {
        tombstone::UploadJob job;
        job.url = kEventsBatchUrl;
        job.body = R"({"sentAtIso":"2026-11-27T00:00:00.000Z","items":[]})";
        job.durability = tombstone::Durability::persist_on_failure;
        job.kind = tombstone::SidecarKind::event_batch;
        job.sign_body = true;
        worker->enqueue(std::move(job));
    }

    /** A log-pull fulfilment: unsigned, never persisted. */
    void enqueue_pull_request() {
        tombstone::UploadJob job;
        job.url = kPullRequestUrl;
        job.body = R"({"sessionId":"s"})";
        job.durability = tombstone::Durability::persist_on_failure;
        job.no_persist = true;
        worker->enqueue(std::move(job));
    }

    std::size_t files(const char *kind_dir) const {
        std::size_t count = 0;
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator{pending / kind_dir, ec}) {
            if (entry.is_regular_file(ec)) {
                ++count;
            }
        }
        return count;
    }

private:
    static tombstone::UploadJob crash_job() {
        tombstone::UploadJob job;
        job.url = kCrashUrl;
        job.body = R"({"signature":"s","stackHint":"h","log":false})";
        job.durability = tombstone::Durability::write_ahead;
        job.kind = tombstone::SidecarKind::crash;
        job.sign_body = true;
        return job;
    }
};

}  // namespace worker_rig

#endif  // TOMBSTONE_TESTS_WORKER_RIG_H
