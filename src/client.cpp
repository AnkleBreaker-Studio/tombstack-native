#include "client.h"

#include "clock.h"
#include "json_scan.h"
#include "json_writer.h"
#include "payloads.h"
#include "platform.h"
#include "transport.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

namespace fs = std::filesystem;

namespace tombstone {

namespace {

constexpr const char *crashes_path = "/api/v1/ingest/crashes";
constexpr const char *bug_reports_path = "/api/v1/ingest/bug-reports";
constexpr const char *events_path = "/api/v1/ingest/events";
constexpr const char *heartbeats_path = "/api/v1/ingest/heartbeats";
constexpr const char *events_batch_path = "/api/v1/ingest/events:batch";
constexpr const char *metrics_batch_path = "/api/v1/ingest/metrics:batch";
constexpr const char *pull_requests_path = "/api/v1/pull-requests";

constexpr int min_heartbeat_interval_s = 15;
constexpr int max_heartbeat_interval_s = 600;
constexpr int default_heartbeat_interval_s = 60;

constexpr const char *unclean_signature = "unclean-shutdown";
constexpr const char *unclean_stack_hint =
    "Previous session ended without a clean shutdown (hard crash, OOM kill, or force quit)";

std::string random_session_id() {
    std::mt19937_64 engine{std::random_device{}()};
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (int chunk = 0; chunk < 2; ++chunk) {
        const std::uint64_t value = engine();
        for (unsigned i = 0; i < 16; ++i) {
            out += digits[(value >> (i * 4U)) & 0xFU];
        }
    }
    return out;
}

const char *ingest_path_for(SidecarKind kind) noexcept {
    switch (kind) {
    case SidecarKind::crash:
        return crashes_path;
    case SidecarKind::bug_report:
        return bug_reports_path;
    case SidecarKind::event:
    default:
        return events_path;
    }
}

std::string trim_trailing_slash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

}  // namespace

Client::Client() = default;

Client::~Client() {
    try {
        stop_heartbeat();
        if (session_log_enabled_ && storage_available_) {
            session_log_.flush_now();
        }
        if (worker_) {
            // Quit flush (spec section 16): drain buffered events/metrics and
            // give the worker a bounded window to deliver before we tear down.
            flush_all_batches();
            worker_->flush(std::chrono::milliseconds{2000});
            worker_->stop();  // persists durable leftovers as sidecars
        }
        if (storage_available_) {
            marker_.remove();  // clean shutdown: next launch sees no dirty marker
        }
    } catch (...) {
        // Destructor must never throw.
    }
}

tombstone_result Client::init(const tombstone_options &options) {
    if (options.endpoint == nullptr || options.endpoint[0] == '\0' ||
        options.token == nullptr || options.token[0] == '\0' ||
        options.build_version == nullptr || options.build_version[0] == '\0') {
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;
    }

    sdk_log_.configure(options.log_callback, options.log_callback_user_data);

    endpoint_ = trim_trailing_slash(options.endpoint);
    token_ = options.token;
    build_version_ = options.build_version;
    os_ = os_name();
    arch_ = arch_name();
    session_id_ = random_session_id();
    heartbeats_enabled_ = options.enable_heartbeats != 0;
    session_log_enabled_ = options.enable_session_log != 0;
    unclean_detection_enabled_ = options.enable_unclean_shutdown_detection != 0;
    rtt_metric_enabled_ = options.enable_rtt_metric != 0;
    consent_ = options.consent_granted != 0;

    const int interval = options.heartbeat_interval_s > 0 ? options.heartbeat_interval_s
                                                          : default_heartbeat_interval_s;
    heartbeat_interval_ = std::chrono::seconds{
        std::clamp(interval, min_heartbeat_interval_s, max_heartbeat_interval_s)};

    data_dir_ = (options.data_dir != nullptr && options.data_dir[0] != '\0')
                    ? fs::path{options.data_dir}
                    : default_data_dir();
    configure_storage();

    transport_ = std::make_unique<Transport>(sdk_log_);
    worker_ = std::make_unique<Worker>(*transport_, sidecars_, session_log_, sdk_log_, token_);
    worker_->set_batch_drainer([this] { drain_ready_batches(); });
    worker_->set_ack_handler([this](const std::string &body) { handle_heartbeat_ack(body); });
    if (rtt_metric_enabled_) {
        worker_->set_rtt_handler([this](double ms) { record_rtt_metric(ms); });
    }
    worker_->start();

    initialized_ = true;
    drain_sidecars();

    if (heartbeats_enabled_) {
        heartbeat_thread_ = std::thread{[this] { heartbeat_loop(); }};
    }
    if (consent_) {
        start_session_tracking();
    }
    return TOMBSTONE_OK;
}

/** Resolve the data dir and wire up the disk-backed subsystems. Degrades to
 * memory-only operation (no sidecars, no session log) when the dir is unusable. */
void Client::configure_storage() {
    try {
        std::error_code ec;
        fs::create_directories(data_dir_, ec);
        if (ec && !fs::exists(data_dir_, ec)) {
            sdk_log_.warn("data dir unavailable; running without offline persistence");
            return;
        }
        storage_available_ = true;
        sidecars_.configure(data_dir_);
        marker_.configure(data_dir_);
        if (session_log_enabled_) {
            session_log_.configure(data_dir_);
            had_previous_log_ = session_log_.rotate_for_new_session();
        }
        if (unclean_detection_enabled_) {
            previous_marker_ = marker_.take_previous();
        } else {
            marker_.remove();  // never leave a stale marker behind
        }
    } catch (const std::exception &e) {
        storage_available_ = false;
        sdk_log_.warn(std::string{"storage setup failed: "} + e.what());
    }
}

/** Re-queue payloads persisted by an earlier run. Restored records came from
 * a previous session: a granted log presign must upload that run's preserved
 * log, never this session's fresh one. */
void Client::drain_sidecars() {
    for (SidecarRecord &record : sidecars_.scan()) {
        if (record.kind == SidecarKind::crash) {
            has_restored_crash_ = true;
        }
        UploadJob job;
        job.url = endpoint_ + ingest_path_for(record.kind);
        job.body = std::move(record.body);
        job.durability = Durability::write_ahead;
        job.kind = record.kind;
        job.sidecar_path = record.path;
        job.request_log = (record.kind != SidecarKind::event) &&
                          find_bool_field(job.body, "log").value_or(false);
        job.log_from_previous = true;
        job.sign_body = true;  // restored crash/bug/event sidecars are ingest POSTs (S3)
        worker_->enqueue(std::move(job));
    }
}

/**
 * Start dirty-session tracking exactly once per launch, the first time capture
 * is allowed (at init, or at set_consent(true) when consent was deferred):
 * write this session's marker and report the previous unclean shutdown, if any.
 */
void Client::start_session_tracking() {
    {
        const std::lock_guard<std::mutex> lock(session_tracking_mutex_);
        if (session_tracking_started_ || !unclean_detection_enabled_ || !storage_available_) {
            return;
        }
        session_tracking_started_ = true;
    }
    marker_.write(SessionMarkerData{session_id_, now_iso8601_utc_ms(), build_version_, os_, arch_});
    if (previous_marker_.has_value()) {
        const SessionMarkerData previous = *previous_marker_;
        previous_marker_.reset();
        report_unclean_shutdown(previous);
    }
}

/**
 * The previous run left its marker behind -> it died hard. No double counting:
 * when a crash sidecar from that session was restored, the death is already
 * represented (and that report's log presign carries previous-session.log).
 * Only when no crash survived do we send this synthetic report.
 */
void Client::report_unclean_shutdown(const SessionMarkerData &previous) {
    if (has_restored_crash_) {
        return;
    }
    CrashPayload payload;
    payload.occurred_at_iso = now_iso8601_utc_ms();  // detection time
    payload.build_version = previous.build_version.empty() ? build_version_ : previous.build_version;
    payload.os = previous.os.empty() ? os_ : previous.os;
    payload.arch = previous.arch.empty() ? arch_ : previous.arch;
    payload.signature = unclean_signature;  // constant -> all unclean shutdowns group together
    payload.stack_hint = unclean_stack_hint;
    payload.user_id = current_user_id();
    payload.steam_id = current_steam_id();
    // This launch's crumbs belong to this session, not the dead one.
    payload.log = session_log_enabled_ && had_previous_log_;
    enqueue_ingest(crashes_path, build_crash_json(payload), Durability::write_ahead,
                   SidecarKind::crash, payload.log, /*log_from_previous=*/true);
}

void Client::heartbeat_loop() {
    std::unique_lock<std::mutex> lock(heartbeat_mutex_);
    while (!heartbeat_stop_) {
        if (capture_allowed()) {
            try {
                HeartbeatPayload payload;
                payload.session_id = session_id_;
                payload.occurred_at_iso = now_iso8601_utc_ms();
                payload.build_version = build_version_;
                payload.os = os_;
                payload.arch = arch_;
                payload.user_id = current_user_id();
                // role/serverId/matchId let the server register Fleet servers and
                // honor match/server log-pulls. role is always present: default to
                // "client" until a match marks this actor a "server".
                const MatchContext ctx = current_match_context();
                payload.role = ctx.role.empty() ? std::string{"client"} : ctx.role;
                payload.server_id = ctx.server_id;
                payload.match_id = ctx.match_id;
                UploadJob job;
                job.url = endpoint_ + heartbeats_path;
                job.body = build_heartbeat_json(payload);
                job.durability = Durability::ephemeral;  // a missed beat is stale data
                job.parse_ack = true;  // the 202 ack may carry pull requests for us
                job.sign_body = true;  // heartbeats are an ingest POST — sign them (S3)
                worker_->enqueue(std::move(job));
            } catch (const std::exception &e) {
                sdk_log_.warn(std::string{"heartbeat build failed: "} + e.what());
            }
        }
        heartbeat_cv_.wait_for(lock, heartbeat_interval_, [this] { return heartbeat_stop_; });
    }
}

void Client::stop_heartbeat() {
    {
        const std::lock_guard<std::mutex> lock(heartbeat_mutex_);
        heartbeat_stop_ = true;
    }
    heartbeat_cv_.notify_all();
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
}

/**
 * Heartbeat command channel: for each pending pull request in the ack that
 * targets THIS client (by userId/sessionId/matchId/serverId) — and only while
 * consent is granted — POST a fulfilment that asks the server to presign a log
 * slot, with request_log set so the existing logUpload chase PUTs the current
 * session log off-thread (spec section 15). Runs on the worker thread; fail-soft
 * throughout — a non-targeted or non-consented client uploads nothing.
 */
void Client::handle_heartbeat_ack(const std::string &response_body) {
    try {
        if (!capture_allowed() || response_body.empty()) {
            return;
        }
        const std::vector<PendingPullRequest> pending = find_pending_requests(response_body);
        if (pending.empty()) {
            return;
        }
        const std::string user_id = current_user_id();
        const MatchContext ctx = current_match_context();
        for (const PendingPullRequest &request : pending) {
            if (request.request_id.empty()) {
                continue;
            }
            if (!pull_request_targets_client(request.target_type, request.target_value, user_id,
                                             session_id_, ctx.match_id, ctx.server_id)) {
                continue;
            }
            UploadJob job;
            job.url = endpoint_ + pull_requests_path + "/" + request.request_id + "/fulfill";
            job.body = build_pull_fulfill_json(user_id, session_id_, ctx.match_id, ctx.server_id,
                                               request.fulfill_nonce, request.nonce_expiry);
            job.durability = Durability::persist_on_failure;  // retried in-session with backoff
            job.request_log = true;  // chase data.logUpload -> PUT the current session log
            job.no_persist = true;   // the presign is time-sensitive; never sidecar across launches
            worker_->enqueue(std::move(job));
        }
    } catch (const std::exception &e) {
        sdk_log_.warn(std::string{"heartbeat ack handling failed: "} + e.what());
    } catch (...) {
        sdk_log_.warn("heartbeat ack handling failed (unknown error)");
    }
}

tombstone_result Client::set_consent(bool granted) {
    const bool was_granted = consent_.exchange(granted);
    if (granted && initialized_) {
        start_session_tracking();
    } else if (!granted && was_granted) {
        // Consent revoked: purge buffered breadcrumbs so the pre-revoke trail can't
        // attach to a crash captured after consent is re-granted (GDPR scoping).
        breadcrumbs_.clear();
    }
    return TOMBSTONE_OK;
}

tombstone_result Client::set_match_context(const char *server_id, const char *match_id) {
    const std::lock_guard<std::mutex> lock(match_mutex_);
    server_id_ = (server_id != nullptr)
                     ? std::string{utf8_safe_truncate(server_id, limits::server_id)}
                     : std::string{};
    match_id_ = (match_id != nullptr)
                    ? std::string{utf8_safe_truncate(match_id, limits::match_id)}
                    : std::string{};
    return TOMBSTONE_OK;
}

tombstone_result Client::start_match(char *out_match_id, std::size_t out_cap) {
    if (out_match_id == nullptr || out_cap == 0) {
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;
    }
    const std::string id = random_session_id();  // 32 hex chars, same generator as session ids
    if (out_cap < id.size() + 1) {
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;  // buffer too small: cached context unchanged
    }
    {
        const std::lock_guard<std::mutex> lock(match_mutex_);
        role_ = "server";
        match_id_ = id;
    }
    id.copy(out_match_id, id.size());
    out_match_id[id.size()] = '\0';
    return TOMBSTONE_OK;
}

tombstone_result Client::end_match() {
    const std::lock_guard<std::mutex> lock(match_mutex_);
    match_id_.clear();  // role + server id are retained across matches
    return TOMBSTONE_OK;
}

Client::MatchContext Client::current_match_context() const {
    const std::lock_guard<std::mutex> lock(match_mutex_);
    return MatchContext{role_, server_id_, match_id_};
}

tombstone_result Client::flush(int timeout_ms) {
    if (!initialized_ || !worker_) {
        return TOMBSTONE_ERROR_NOT_INITIALIZED;
    }
    if (session_log_enabled_ && storage_available_) {
        session_log_.flush_now();
    }
    flush_all_batches();  // buffered events/metrics join the outbound queue
    const auto timeout = std::chrono::milliseconds{timeout_ms > 0 ? timeout_ms : 0};
    return worker_->flush(timeout) ? TOMBSTONE_OK : TOMBSTONE_ERROR_TIMEOUT;
}

void Client::maybe_flush_batch(Batch &batch, const char *path,
                               std::chrono::steady_clock::time_point now, bool force,
                               bool suppress_rtt) {
    if (!batch.has_items()) {
        return;  // cheap short-circuit: an empty idle loop allocates nothing (section 15)
    }
    std::optional<std::string> envelope = batch.drain_if_ready(now_iso8601_utc_ms(), now, force);
    if (!envelope.has_value()) {
        return;
    }
    UploadJob job;
    job.url = endpoint_ + path;
    job.body = std::move(*envelope);
    job.durability = Durability::persist_on_failure;  // retried with backoff in-session
    job.no_persist = true;  // a batch envelope is not a single-item sidecar
    job.sign_body = true;  // events:batch / metrics:batch are ingest POSTs — sign them (S3)
    job.suppress_rtt = suppress_rtt;  // the metrics batch's own upload must not emit an rtt metric
    // Record the flush time for diagnostics (K3); steady-clock ns, 0 means "never".
    last_flush_steady_ns_.store(
        std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    worker_->enqueue(std::move(job));
}

void Client::drain_ready_batches() {
    const auto now = std::chrono::steady_clock::now();
    maybe_flush_batch(event_batch_, events_batch_path, now, /*force=*/false, /*suppress_rtt=*/false);
    maybe_flush_batch(metric_batch_, metrics_batch_path, now, /*force=*/false, /*suppress_rtt=*/true);
}

void Client::flush_all_batches() {
    const auto now = std::chrono::steady_clock::now();
    maybe_flush_batch(event_batch_, events_batch_path, now, /*force=*/true, /*suppress_rtt=*/false);
    maybe_flush_batch(metric_batch_, metrics_batch_path, now, /*force=*/true, /*suppress_rtt=*/true);
}

}  // namespace tombstone
