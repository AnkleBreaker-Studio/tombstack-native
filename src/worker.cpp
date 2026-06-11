#include "worker.h"

#include "json_scan.h"
#include "sdk_log.h"
#include "session_log.h"
#include "transport.h"

#include <utility>

namespace tombstone {

namespace {

/** Mirrors tools/lib/upload-classify.mjs: 2xx delete, 429/5xx keep, other 4xx drop. */
enum class Outcome { delivered, transient, poison };

Outcome classify(bool transport_error, long status) noexcept {
    if (transport_error) {
        return Outcome::transient;
    }
    if (status >= 200 && status < 300) {
        return Outcome::delivered;
    }
    if (status == 408 || status == 429 || status >= 500) {
        return Outcome::transient;
    }
    return Outcome::poison;
}

}  // namespace

Worker::Worker(Transport &transport, SidecarQueue &sidecars, SessionLog &session_log,
               SdkLog &sdk_log, std::string token)
    : transport_(transport),
      sidecars_(sidecars),
      session_log_(session_log),
      sdk_log_(sdk_log),
      token_(std::move(token)) {}

Worker::~Worker() { stop(); }

void Worker::start() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return;
    }
    running_ = true;
    next_log_flush_ = std::chrono::steady_clock::now() + log_flush_interval;
    thread_ = std::thread{[this] { run(); }};
}

void Worker::stop() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    persist_leftovers();
}

void Worker::set_batch_drainer(std::function<void()> drainer) {
    batch_drainer_ = std::move(drainer);  // set before start(); no concurrent access yet
}

void Worker::wake() {
    wake_requested_.store(true);
    cv_.notify_all();
}

void Worker::enqueue(UploadJob job) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= max_queue_size) {
            queue_.pop_front();  // bounded: drop the oldest entry
            sdk_log_.warn("outbound queue full; oldest payload dropped");
        }
        queue_.push_back(std::move(job));
    }
    cv_.notify_all();
}

bool Worker::flush(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    return cv_.wait_until(lock, deadline,
                          [this] { return queue_.empty() && !in_flight_; });
}

void Worker::run() {
    for (;;) {
        if (batch_drainer_) {
            // Off the caller's thread: flush count/age-ready batches (spec
            // section 16). Runs unlocked because draining enqueues new jobs.
            batch_drainer_();
        }
        UploadJob job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_until(lock, next_wakeup(), [this] {
                if (!running_ || wake_requested_.load()) {
                    return true;
                }
                const auto now = std::chrono::steady_clock::now();
                for (const UploadJob &queued : queue_) {
                    if (queued.not_before <= now) {
                        return true;
                    }
                }
                return now >= next_log_flush_;
            });
            if (!running_) {
                return;
            }
            wake_requested_.store(false);
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_log_flush_) {
                next_log_flush_ = now + log_flush_interval;
                lock.unlock();
                session_log_.flush_now();  // paced flush, off the game thread
                lock.lock();
            }
            if (!take_ready_job(job)) {
                continue;
            }
            in_flight_ = true;
        }
        process(job);
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            in_flight_ = false;
        }
        cv_.notify_all();  // wake flush() waiters
    }
}

bool Worker::take_ready_job(UploadJob &out) {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->not_before <= now) {
            out = std::move(*it);
            queue_.erase(it);
            return true;
        }
    }
    return false;
}

std::chrono::steady_clock::time_point Worker::next_wakeup() const {
    auto wakeup = next_log_flush_;
    for (const UploadJob &queued : queue_) {
        if (queued.not_before < wakeup) {
            wakeup = queued.not_before;
        }
    }
    if (batch_drainer_) {
        // Bound the idle wait so the age trigger is evaluated within ~1s.
        const auto batch_deadline = std::chrono::steady_clock::now() + batch_check_interval;
        if (batch_deadline < wakeup) {
            wakeup = batch_deadline;
        }
    }
    return wakeup;
}

void Worker::process(UploadJob &job) {
    try {
        if (job.is_log_put) {
            const HttpResponse response =
                transport_.put_text(job.url, job.raw, log_put_timeout_seconds);
            const Outcome outcome = classify(response.transport_error, response.status);
            if (outcome == Outcome::transient && job.attempt + 1 < max_attempts) {
                // Retries share the backoff but log PUTs are never persisted —
                // the presigned URL is dead by the next launch anyway.
                retry_or_give_up(job);
            } else if (outcome == Outcome::poison) {
                sdk_log_.warn("log upload rejected with HTTP " + std::to_string(response.status));
            }
            return;
        }
        const HttpResponse response =
            transport_.post_json(job.url, token_, job.body, request_timeout_seconds);
        handle_post_result(job, response.transport_error, response.status, response.body);
    } catch (const std::exception &e) {
        sdk_log_.warn(std::string{"upload bookkeeping failed: "} + e.what());
    } catch (...) {
        sdk_log_.warn("upload bookkeeping failed (unknown error)");
    }
}

void Worker::handle_post_result(UploadJob &job, bool transport_error, long status,
                                const std::string &response_body) {
    switch (classify(transport_error, status)) {
    case Outcome::delivered:
        sidecars_.remove(job.sidecar_path);
        if (job.request_log) {
            schedule_log_put(job, response_body);
        }
        return;
    case Outcome::poison:
        // Rejected by validation/auth — retrying forever would just burn quota.
        sdk_log_.warn("payload rejected with HTTP " + std::to_string(status) + "; dropping");
        sidecars_.remove(job.sidecar_path);
        return;
    case Outcome::transient:
    default:
        if (job.durability == Durability::ephemeral) {
            return;  // a missed heartbeat is stale data, never retried
        }
        retry_or_give_up(job);
        return;
    }
}

void Worker::schedule_log_put(const UploadJob &job, const std::string &response_body) {
    const std::optional<std::string> url = find_log_upload_url(response_body);
    if (!url.has_value() || url->empty()) {
        return;  // server granted no slot — fail-soft, the report itself landed
    }
    if (job.log_from_previous && previous_log_claimed_.exchange(true)) {
        return;  // previous-session.log already uploaded (or in flight) this launch
    }
    std::vector<char> bytes =
        job.log_from_previous ? session_log_.read_previous() : session_log_.read_current();
    if (bytes.empty()) {
        return;
    }
    UploadJob put;
    put.is_log_put = true;
    put.url = *url;
    put.raw = std::move(bytes);
    put.durability = Durability::persist_on_failure;  // retried, never persisted
    enqueue(std::move(put));
}

void Worker::retry_or_give_up(UploadJob &job) {
    if (job.attempt + 1 < max_attempts) {
        ++job.attempt;
        const auto delay = retry_base_delay * (1 << (job.attempt - 1));  // 2s,4s,8s,16s
        job.not_before = std::chrono::steady_clock::now() + delay;
        enqueue(std::move(job));
        return;
    }
    // Final in-session failure: make sure durable payloads survive to the next
    // launch (write-ahead jobs already have a backing file). Batch envelopes are
    // not single-item sidecars (the uploader posts those to the non-batch
    // endpoint), so they are dropped fail-soft instead of mis-persisted.
    if (!job.is_log_put && !job.no_persist && job.sidecar_path.empty() &&
        job.durability != Durability::ephemeral) {
        sidecars_.write(job.kind, job.body);
    }
}

void Worker::persist_leftovers() {
    std::deque<UploadJob> leftovers;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        leftovers.swap(queue_);
    }
    for (const UploadJob &job : leftovers) {
        if (!job.is_log_put && !job.no_persist && job.sidecar_path.empty() &&
            job.durability != Durability::ephemeral) {
            sidecars_.write(job.kind, job.body);
        }
    }
}

}  // namespace tombstone
