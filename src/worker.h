#ifndef TOMBSTONE_SRC_WORKER_H
#define TOMBSTONE_SRC_WORKER_H

#include "sidecar_queue.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tombstone {

class SdkLog;
class SessionLog;
class Transport;

/** How hard the SDK fights to deliver a payload (mirrors the Unity SDK). */
enum class Durability {
    /** Best-effort, time-sensitive (heartbeats): one attempt, never persisted. */
    ephemeral,
    /** Retried with backoff in-session; persisted to disk after the final failure (events). */
    persist_on_failure,
    /** Persisted BEFORE the first attempt so a quit/crash cannot lose it (crashes, bugs). */
    write_ahead,
};

/** One outbound item: a JSON ingest POST, or a raw session-log PUT. */
struct UploadJob {
    bool is_log_put{false};
    std::string url;          // absolute URL (ingest endpoint or presigned PUT)
    std::string body;         // JSON body (POST jobs)
    std::vector<char> raw;    // log bytes (PUT jobs)
    Durability durability{Durability::persist_on_failure};
    SidecarKind kind{SidecarKind::event};
    std::filesystem::path sidecar_path;  // non-empty when backed by a pending file
    bool request_log{false};             // body carries "log":true -> chase the presign on 2xx
    bool log_from_previous{false};       // a granted presign uploads previous-session.log
    bool no_persist{false};              // batch envelopes: retry in-session, never sidecar'd
    int attempt{0};
    std::chrono::steady_clock::time_point not_before{};
};

/**
 * Background delivery thread: drains a bounded queue (drop-oldest), uploads
 * with in-session exponential backoff (2s -> 32s, 5 attempts), classifies
 * results (2xx delivered / poison 4xx dropped / 429 + 5xx + network retried),
 * persists durable leftovers as sidecars, chases presigned session-log PUTs,
 * and paces the rolling session-log flush. Owns no payload semantics — the
 * client builds bodies; the worker delivers them.
 */
class Worker {
public:
    static constexpr std::size_t max_queue_size = 256;
    static constexpr int max_attempts = 5;
    static constexpr std::chrono::seconds retry_base_delay{2};
    static constexpr long request_timeout_seconds = 15;
    static constexpr long log_put_timeout_seconds = 30;
    static constexpr std::chrono::seconds log_flush_interval{5};
    /** Idle wake cadence while batching is active so the age trigger is seen
     *  within ~1s (timed wait, never a busy loop -- spec section 15). */
    static constexpr std::chrono::seconds batch_check_interval{1};

    Worker(Transport &transport, SidecarQueue &sidecars, SessionLog &session_log,
           SdkLog &sdk_log, std::string token);
    ~Worker();

    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;
    Worker(Worker &&) = delete;
    Worker &operator=(Worker &&) = delete;

    void start();

    /** Stop the thread; durable undelivered jobs are persisted as sidecars. */
    void stop();

    /** Queue a job (thread-safe). Oldest entry is dropped when full. */
    void enqueue(UploadJob job);

    /** Install the batch-drain callback, invoked on each worker wake (off the
     *  caller's thread) to flush count/age-ready event/metric batches. Must be
     *  set before start() (single-threaded init); the worker owns no batch
     *  semantics, it just gives them a thread to run on. */
    void set_batch_drainer(std::function<void()> drainer);

    /** Nudge the worker to run the batch drainer now (count trigger). */
    void wake();

    /** True when the queue drained (and nothing is in flight) within the timeout. */
    bool flush(std::chrono::milliseconds timeout);

private:
    void run();
    bool take_ready_job(UploadJob &out);
    std::chrono::steady_clock::time_point next_wakeup() const;
    void process(UploadJob &job);
    void handle_post_result(UploadJob &job, bool transport_error, long status,
                            const std::string &response_body);
    void schedule_log_put(const UploadJob &job, const std::string &response_body);
    void retry_or_give_up(UploadJob &job);
    void persist_leftovers();

    Transport &transport_;
    SidecarQueue &sidecars_;
    SessionLog &session_log_;
    SdkLog &sdk_log_;
    const std::string token_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<UploadJob> queue_;
    bool running_{false};
    bool in_flight_{false};
    std::atomic<bool> previous_log_claimed_{false};
    std::atomic<bool> wake_requested_{false};
    std::function<void()> batch_drainer_;  // set before start(); read on the worker thread
    std::chrono::steady_clock::time_point next_log_flush_{};
    std::thread thread_;
};

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_WORKER_H
