#include "worker.h"

#include "json_scan.h"
#include "retry_after.h"
#include "sdk_log.h"
#include "session_log.h"
#include "transport.h"

#include <utility>

namespace tombstone {

namespace {

/** 2xx delete, 408/429/5xx/network keep, a signature 401 keep (retry with the
 *  corrected clock), any other 4xx drop (a 401 refusing the token included). */
enum class Outcome { delivered, transient, poison };

Outcome classify(bool transport_error, long status, Unauthorized unauthorized) noexcept {
    if (transport_error) {
        return Outcome::transient;
    }
    if (status >= 200 && status < 300) {
        return Outcome::delivered;
    }
    if (unauthorized == Unauthorized::signature) {
        return Outcome::transient;  // a wrong device clock, corrected from this response's Date
    }
    if (status == 408 || status == 429 || status >= 500) {
        return Outcome::transient;
    }
    return Outcome::poison;
}

/** How a 401 reads for this job: only a SIGNED request can fail on its signature. */
Unauthorized unauthorized_kind(const UploadJob &job, const HttpResponse &response) noexcept {
    if (response.transport_error || response.status != 401) {
        return Unauthorized::none;
    }
    return job.sign_body ? classify_unauthorized(response.status, response.body)
                         : Unauthorized::key;
}

/** Honor Retry-After only where the server means "back off and come back":
 *  429 (rate limited) and 503 (unavailable). -1 = no usable hint. */
long retry_after_hint(long status, const std::string &retry_after) noexcept {
    if (status != 429 && status != 503) {
        return -1;
    }
    return parse_retry_after_seconds(retry_after);
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
    // Cancel a request still on the wire so the join below never waits out a
    // network timeout (up to 30 s for a log upload): the aborted job comes back
    // as a transport error and is persisted with the other leftovers.
    transport_.abort_inflight();
    if (thread_.joinable()) {
        thread_.join();
    }
    persist_leftovers();
}

void Worker::set_batch_drainer(std::function<void()> drainer) {
    batch_drainer_ = std::move(drainer);  // set before start(); no concurrent access yet
}

void Worker::set_ack_handler(std::function<void(const std::string &)> handler) {
    ack_handler_ = std::move(handler);  // set before start(); no concurrent access yet
}

void Worker::set_rtt_handler(std::function<void(double)> handler) {
    rtt_handler_ = std::move(handler);  // set before start(); no concurrent access yet
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

std::size_t Worker::queued_count() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

std::uint64_t Worker::consent_epoch() const noexcept { return consent_epoch_.load(); }

void Worker::revoke_consent() {
    std::deque<UploadJob> dropped;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        consent_epoch_.fetch_add(1);
        for (auto it = queue_.begin(); it != queue_.end();) {
            if (it->consent_gated) {
                dropped.push_back(std::move(*it));
                it = queue_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const UploadJob &job : dropped) {
        sidecars_.remove(job.sidecar_path);
    }
    cv_.notify_all();  // flush() waiters see the emptied queue
}

bool Worker::captured_before_revoke(const UploadJob &job) const noexcept {
    return job.consent_gated && job.consent_epoch != consent_epoch_.load();
}

void Worker::run() {
    for (;;) {
        if (batch_drainer_) {
            // Off the caller's thread: flush count/age-ready batches (spec
            // section 16). Runs unlocked because draining enqueues new jobs.
            // Fail-soft like process(): an exception escaping the thread entry
            // would call std::terminate and crash the host — never acceptable.
            try {
                batch_drainer_();
            } catch (...) {
                sdk_log_.warn("batch drain failed");
            }
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
        if (captured_before_revoke(job)) {
            sidecars_.remove(job.sidecar_path);  // consent was revoked after it was captured
            return;
        }
        if (job.is_log_upload) {
            const HttpResponse response =
                job.form_fields.empty()
                    ? transport_.put_text(job.url, job.raw, log_upload_timeout_seconds)
                    : transport_.post_log(job.url, job.form_fields, job.raw, log_upload_timeout_seconds);
            const Outcome outcome =
                classify(response.transport_error, response.status, Unauthorized::none);
            if (outcome == Outcome::transient && job.attempt + 1 < max_attempts) {
                // Retries share the backoff but log uploads are never persisted —
                // the presigned URL is dead by the next launch anyway.
                retry_or_give_up(job, retry_after_hint(response.status, response.retry_after));
            } else if (outcome == Outcome::poison) {
                sdk_log_.warn("log upload rejected with HTTP " + std::to_string(response.status));
            }
            return;
        }
        const auto sent_at = std::chrono::steady_clock::now();
        const HttpResponse response =
            transport_.post_json(job.url, token_, job.body, request_timeout_seconds, job.sign_body,
                                 server_clock_.now_unix_seconds());
        const auto round_trip = std::chrono::steady_clock::now() - sent_at;
        // Every ingest response carries the server's Date, success or failure: learn the
        // signing clock from it so a device whose clock is wrong signs correctly from now on.
        const bool clock_moved = !response.transport_error && server_clock_.observe(response.date);
        if (clock_moved) {
            sdk_log_.info("device clock differs from the server's; signing with the server time");
        }
        // K1: decide whether to emit the round-trip metric BEFORE handle_post_result,
        // which may move the job into the retry queue. Only successful ingest POSTs
        // count; suppress_rtt exempts the metrics batch so an rtt sample cannot recurse
        // into an endless batch->rtt->batch loop.
        const bool emit_rtt =
            rtt_handler_ && job.sign_body && !job.suppress_rtt &&
            classify(response.transport_error, response.status, Unauthorized::none) ==
                Outcome::delivered;
        handle_post_result(job, response, clock_moved);
        if (emit_rtt) {
            const double ms = std::chrono::duration<double, std::milli>(round_trip).count();
            rtt_handler_(ms);
        }
    } catch (const std::exception &e) {
        sdk_log_.warn(std::string{"upload bookkeeping failed: "} + e.what());
    } catch (...) {
        sdk_log_.warn("upload bookkeeping failed (unknown error)");
    }
}

void Worker::handle_post_result(UploadJob &job, const HttpResponse &response, bool clock_moved) {
    if (captured_before_revoke(job)) {
        // Consent was revoked while this was in flight: no retry, no persistence,
        // no follow-up log upload, whatever the outcome.
        sidecars_.remove(job.sidecar_path);
        return;
    }
    const Unauthorized unauthorized = unauthorized_kind(job, response);
    switch (classify(response.transport_error, response.status, unauthorized)) {
    case Outcome::delivered:
        sidecars_.remove(job.sidecar_path);
        if (job.request_log) {
            schedule_log_upload(job, response.body);
        }
        if (job.parse_ack && ack_handler_) {
            // Heartbeat command channel: hand the 2xx body to the client, which
            // enqueues a fulfil POST for each pull request targeting it.
            ack_handler_(response.body);
        }
        return;
    case Outcome::poison:
        // Rejected by validation/auth — retrying forever would just burn quota.
        sdk_log_.warn("payload rejected with HTTP " + std::to_string(response.status) +
                      "; dropping");
        sidecars_.remove(job.sidecar_path);
        return;
    case Outcome::transient:
    default:
        if (job.durability == Durability::ephemeral) {
            return;  // a missed heartbeat is stale data, never retried
        }
        // A signature refusal whose own Date just corrected the clock is fixed now:
        // retry at once rather than after the backoff.
        retry_or_give_up(job, retry_after_hint(response.status, response.retry_after),
                         clock_moved && unauthorized == Unauthorized::signature);
        return;
    }
}

void Worker::schedule_log_upload(const UploadJob &job, const std::string &response_body) {
    const auto upload = find_log_upload(response_body);
    if (!upload) {
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
    put.is_log_upload = true;
    put.url = upload->url;
    put.form_fields = upload->fields;
    put.raw = std::move(bytes);
    put.durability = Durability::persist_on_failure;  // retried, never persisted
    put.consent_gated = job.consent_gated;            // the log is the report's: same consent
    put.consent_epoch = job.consent_epoch;
    enqueue(std::move(put));
}

void Worker::retry_or_give_up(UploadJob &job, long retry_after_seconds, bool retry_now) {
    if (job.attempt + 1 < max_attempts) {
        ++job.attempt;
        // Exponential backoff (2s,4s,8s,16s), raised to the server's Retry-After
        // (429/503) when that is larger, capped at 300s (retry_after.h).
        const auto now = std::chrono::steady_clock::now();
        job.not_before = retry_now ? now : now + retry_delay(job.attempt, retry_after_seconds);
        enqueue(std::move(job));
        return;
    }
    // Final in-session failure: make sure durable payloads survive to the next
    // launch (write-ahead jobs already have a backing file). Batch envelopes are
    // persisted whole under their own kinds (events-batch / metrics-batch) and
    // replayed to the batch endpoints; time-sensitive control-plane jobs carry
    // no_persist and are dropped.
    if (!job.is_log_upload && !job.no_persist && job.sidecar_path.empty() &&
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
        if (captured_before_revoke(job)) {
            sidecars_.remove(job.sidecar_path);
            continue;
        }
        if (!job.is_log_upload && !job.no_persist && job.sidecar_path.empty() &&
            job.durability != Durability::ephemeral) {
            sidecars_.write(job.kind, job.body);
        }
    }
}

}  // namespace tombstone
