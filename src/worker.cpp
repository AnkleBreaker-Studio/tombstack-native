#include "worker.h"

#include "delivery_policy.h"
#include "json_scan.h"
#include "retry_after.h"
#include "sdk_log.h"
#include "session_log.h"
#include "transport.h"

#include <utility>

namespace tombstone {

namespace {

bool is_delivered(const HttpResponse &response) noexcept {
    return settle_upload_response(response.transport_error, response.status) ==
           Settle::delivered;
}

/** Kept through a stop: a crash or bug report (write-ahead), or a payload already
 *  on disk. Heartbeats, in-memory batches and pull-request calls are not. */
bool kept_through_stop(const UploadJob &job) noexcept {
    return !job.is_log_upload && !job.no_persist &&
           (job.durability == Durability::write_ahead || !job.sidecar_path.empty());
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
        std::unique_lock<std::mutex> lock(mutex_);
        if (!running_) {
            // Never started (or already stopped): still keep what is queued — a
            // second call finds the queue empty.
            lock.unlock();
            persist_leftovers();
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
    SetAside set_aside;
    bool dropped_oldest = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!job.is_log_upload && sending_stopped_locked(std::chrono::steady_clock::now())) {
            set_aside_locked(std::move(job), set_aside);
        } else {
            if (queue_.size() >= max_queue_size) {
                queue_.pop_front();  // bounded: drop the oldest entry
                dropped_oldest = true;
            }
            queue_.push_back(std::move(job));
        }
    }
    finish_set_aside(set_aside);
    if (dropped_oldest) {
        // Outside the lock: the diagnostics callback must never run while it is held.
        sdk_log_.warn("outbound queue full; oldest payload dropped");
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
    return queue_.size() + parked_.size();
}

std::uint64_t Worker::consent_epoch() const noexcept { return consent_epoch_.load(); }

void Worker::revoke_consent() {
    std::deque<UploadJob> dropped;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        consent_epoch_.fetch_add(1);
        for (std::deque<UploadJob> *jobs : {&queue_, &parked_}) {
            for (auto it = jobs->begin(); it != jobs->end();) {
                if (it->consent_gated) {
                    dropped.push_back(std::move(*it));
                    it = jobs->erase(it);
                } else {
                    ++it;
                }
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
                if (!parked_.empty() && !sending_stopped_locked(now)) {
                    return true;  // a spending-cap stop just ended
                }
                return now >= next_log_flush_;
            });
            if (!running_) {
                return;
            }
            wake_requested_.store(false);
            const auto now = std::chrono::steady_clock::now();
            resume_parked_locked(now);
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
    if (!parked_.empty() && !token_refused_ && stopped_until_ < wakeup) {
        wakeup = stopped_until_;  // parked reports go out when the spending-cap stop ends
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
            const Settle settle = settle_upload_response(response.transport_error, response.status);
            if (settle == Settle::retry && job.attempt + 1 < max_attempts) {
                // Retries share the backoff but log uploads are never persisted —
                // the presigned URL is dead by the next launch anyway.
                retry_or_give_up(job, retry_after_hint(response.status, response.retry_after));
            } else if (settle == Settle::drop) {
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
            rtt_handler_ && job.sign_body && !job.suppress_rtt && is_delivered(response);
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
    switch (settle_api_response(response.transport_error, response.status, unauthorized,
                                job.sign_body)) {
    case Settle::delivered:
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
    case Settle::drop:
        // The server refuses this payload itself: retrying would just burn quota.
        sdk_log_.warn("payload rejected with HTTP " + std::to_string(response.status) +
                      "; dropping");
        sidecars_.remove(job.sidecar_path);
        return;
    case Settle::spend_cap:
        stop_for_spend_cap(job, response);
        return;
    case Settle::token_refused:
        stop_for_refused_token(job, response.status);
        return;
    case Settle::retry:
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
        for (UploadJob &parked : parked_) {
            leftovers.push_back(std::move(parked));  // kept reports a stop still held
        }
        parked_.clear();
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

void Worker::stop_for_spend_cap(UploadJob &job, const HttpResponse &response) {
    // resumesAt is the server's time: compare it with the server clock, not the device's.
    const std::int64_t pause_seconds = spend_cap_pause_seconds(
        response.body, server_clock_.now_unix_seconds(), min_spend_cap_stop_.count());
    SetAside set_aside;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{pause_seconds};
        if (until > stopped_until_) {
            stopped_until_ = until;
        }
        set_aside_locked(std::move(job), set_aside);
        set_aside_queue_locked(set_aside);
    }
    finish_set_aside(set_aside);
    sdk_log_.warn("spending cap reached (HTTP 402): nothing is sent for " +
                  std::to_string(pause_seconds) +
                  " s; crash and bug reports are kept and sent after that");
    cv_.notify_all();  // flush() waiters see the emptied queue
}

void Worker::stop_for_refused_token(UploadJob &job, long status) {
    SetAside set_aside;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        token_refused_ = true;
        set_aside_locked(std::move(job), set_aside);
        set_aside_queue_locked(set_aside);
        std::deque<UploadJob> parked;
        parked.swap(parked_);
        for (UploadJob &held : parked) {
            set_aside_locked(std::move(held), set_aside);  // now left to their files
        }
    }
    finish_set_aside(set_aside);
    sdk_log_.error("the SDK token was refused (HTTP " + std::to_string(status) +
                   "): nothing more is sent until the next launch; crash and bug reports "
                   "stay on disk");
    cv_.notify_all();
}

bool Worker::sending_stopped_locked(std::chrono::steady_clock::time_point now) const noexcept {
    return token_refused_ || now < stopped_until_;
}

void Worker::set_aside_locked(UploadJob &&job, SetAside &out) {
    if (captured_before_revoke(job)) {
        if (!job.sidecar_path.empty()) {
            out.remove.push_back(job.sidecar_path);
        }
        return;
    }
    if (!kept_through_stop(job)) {
        return;  // a stop is a hard stop: nothing else captured meanwhile is kept for later
    }
    if (!token_refused_ && parked_.size() < max_queue_size) {
        parked_.push_back(std::move(job));  // sent once the spending-cap stop ends
        return;
    }
    if (job.sidecar_path.empty()) {
        out.persist.push_back(std::move(job));  // its file keeps it for a later launch
    }
}

void Worker::set_aside_queue_locked(SetAside &out) {
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->is_log_upload) {
            ++it;  // an upload to a slot granted earlier is not refused by a stop
            continue;
        }
        set_aside_locked(std::move(*it), out);
        it = queue_.erase(it);
    }
}

void Worker::resume_parked_locked(std::chrono::steady_clock::time_point now) {
    if (parked_.empty() || sending_stopped_locked(now)) {
        return;
    }
    for (UploadJob &job : parked_) {
        job.not_before = now;
        if (queue_.size() >= max_queue_size) {
            queue_.pop_front();  // bounded as in enqueue(); a report's file outlives this
        }
        queue_.push_back(std::move(job));
    }
    parked_.clear();
}

void Worker::finish_set_aside(SetAside &work) {
    for (const UploadJob &job : work.persist) {
        sidecars_.write(job.kind, job.body);
    }
    for (const std::filesystem::path &path : work.remove) {
        sidecars_.remove(path);
    }
    work.persist.clear();
    work.remove.clear();
}

}  // namespace tombstone
