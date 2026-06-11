#include "client.h"

#include "clock.h"
#include "json_writer.h"
#include "payloads.h"
#include "signature.h"
#include "transport.h"

#include <chrono>
#include <cmath>

namespace tombstone {

namespace {

constexpr const char *crashes_path = "/api/v1/ingest/crashes";
constexpr const char *bug_reports_path = "/api/v1/ingest/bug-reports";
constexpr const char *pull_requests_path = "/api/v1/pull-requests";

const char *level_label(tombstone_level level) noexcept {
    switch (level) {
    case TOMBSTONE_LEVEL_DEBUG:
        return "Debug";
    case TOMBSTONE_LEVEL_WARN:
        return "Warning";
    case TOMBSTONE_LEVEL_ERROR:
        return "Error";
    case TOMBSTONE_LEVEL_INFO:
    default:
        return "Info";
    }
}

std::string clamped(const char *value, std::size_t max_bytes) {
    if (value == nullptr) {
        return {};
    }
    return std::string{utf8_safe_truncate(value, max_bytes)};
}

}  // namespace

tombstone_result Client::set_user(const char *user_id, const char *steam_id) {
    const std::lock_guard<std::mutex> lock(user_mutex_);
    user_id_ = clamped(user_id, limits::user_id);
    steam_id_ = clamped(steam_id, limits::steam_id);
    return TOMBSTONE_OK;
}

std::string Client::current_user_id() const {
    const std::lock_guard<std::mutex> lock(user_mutex_);
    return user_id_;
}

std::string Client::current_steam_id() const {
    const std::lock_guard<std::mutex> lock(user_mutex_);
    return steam_id_;
}

void Client::record_breadcrumb(std::string_view level, std::string_view message) {
    breadcrumbs_.add(level, message, now_iso8601_utc_ms());
}

void Client::enqueue_ingest(const char *path, std::string body, Durability durability,
                            SidecarKind kind, bool request_log, bool log_from_previous) {
    UploadJob job;
    job.url = endpoint_ + path;
    job.body = std::move(body);
    job.durability = durability;
    job.kind = kind;
    job.request_log = request_log;
    job.log_from_previous = log_from_previous;
    job.sign_body = true;  // crashes/bug-reports/events are ingest POSTs — sign them (S3)
    if (durability == Durability::write_ahead && storage_available_) {
        // Persisted BEFORE the first attempt so a quit/crash cannot lose it.
        job.sidecar_path = sidecars_.write(kind, job.body);
    }
    worker_->enqueue(std::move(job));
}

tombstone_result Client::add_breadcrumb(tombstone_level level, const char *message) {
    if (message == nullptr || message[0] == '\0') {
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;
    }
    if (!capture_allowed()) {
        return TOMBSTONE_DISABLED;
    }
    record_breadcrumb(level_label(level), message);
    return TOMBSTONE_OK;
}

tombstone_result Client::log_line(tombstone_level level, const char *line) {
    if (line == nullptr || line[0] == '\0') {
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;
    }
    if (!capture_allowed()) {
        return TOMBSTONE_DISABLED;
    }
    const char *label = level_label(level);
    if (session_log_enabled_ && storage_available_) {
        session_log_.append(now_iso8601_utc_ms(), label, line);
    }
    // Every mirrored line is also a breadcrumb (the crash-trail source).
    record_breadcrumb(label, line);
    return TOMBSTONE_OK;
}

tombstone_result Client::track_event(const char *name, const char *const *keys,
                                     const char *const *values, std::size_t count) {
    if (name == nullptr || name[0] == '\0') {
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;
    }
    if (count > 0 && (keys == nullptr || values == nullptr)) {
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;
    }
    if (!capture_allowed()) {
        return TOMBSTONE_DISABLED;
    }
    EventPayload payload;
    payload.occurred_at_iso = now_iso8601_utc_ms();
    payload.build_version = build_version_;
    payload.os = os_;
    payload.arch = arch_;
    payload.name = name;
    payload.user_id = current_user_id();
    const MatchContext event_ctx = current_match_context();
    payload.role = event_ctx.role;
    payload.server_id = event_ctx.server_id;
    payload.match_id = event_ctx.match_id;
    payload.session_id = session_id_;
    payload.attributes.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (keys[i] == nullptr || keys[i][0] == '\0') {
            continue;  // skip unusable entries; null values become ""
        }
        payload.attributes.emplace_back(keys[i], values[i] != nullptr ? values[i] : "");
    }
    // Batched (spec section 16): the worker drains the envelope off this thread
    // on count/age/quit. A count trigger wakes it so a burst flushes promptly.
    if (event_batch_.add(build_event_json(payload))) {
        worker_->wake();
    }
    return TOMBSTONE_OK;
}

tombstone_result Client::track_metric(const char *name, double value, const char *unit) {
    if (name == nullptr || name[0] == '\0') {
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;
    }
    if (!std::isfinite(value)) {
        // NaN/Infinity would corrupt aggregation and is rejected by the metric
        // schema; drop it at the boundary rather than ship invalid JSON.
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;
    }
    if (!capture_allowed()) {
        return TOMBSTONE_DISABLED;
    }
    MetricPayload payload;
    payload.name = name;
    payload.value = value;
    payload.unit = (unit != nullptr) ? unit : "";
    payload.occurred_at_iso = now_iso8601_utc_ms();
    payload.build_version = build_version_;
    payload.os = os_;
    payload.arch = arch_;
    payload.user_id = current_user_id();
    const MatchContext metric_ctx = current_match_context();
    payload.role = metric_ctx.role;
    payload.server_id = metric_ctx.server_id;
    payload.match_id = metric_ctx.match_id;
    payload.session_id = session_id_;
    if (metric_batch_.add(build_metric_json(payload))) {
        worker_->wake();
    }
    return TOMBSTONE_OK;
}

tombstone_result Client::report_crash(const char *signature, const char *stack_hint,
                                      const char *stack_trace, bool attach_log) {
    if (!capture_allowed()) {
        return TOMBSTONE_DISABLED;
    }
    // stackHint has a server-side min(1): never send it empty.
    const std::string hint =
        (stack_hint != nullptr && stack_hint[0] != '\0') ? stack_hint : "Crash";
    const std::string trace = stack_trace != nullptr ? stack_trace : "";
    const std::string effective_signature = (signature != nullptr && signature[0] != '\0')
                                                ? std::string{signature}
                                                : compute_crash_signature(hint, trace);

    const DedupeWindow::Decision decision =
        dedupe_.check(effective_signature, std::chrono::steady_clock::now());
    if (!decision.allowed) {
        // Repeats ride the breadcrumb trail instead of burning quota.
        record_breadcrumb("Error", "crash suppressed (duplicate x" +
                                       std::to_string(decision.suppressed_count) +
                                       " within 60s): " + hint);
        return TOMBSTONE_SUPPRESSED;
    }

    const bool want_log = attach_log && session_log_enabled_ && storage_available_;
    if (want_log) {
        session_log_.append(now_iso8601_utc_ms(), "Crash", hint + "\n" + trace);
    }

    CrashPayload payload;
    payload.occurred_at_iso = now_iso8601_utc_ms();
    payload.build_version = build_version_;
    payload.os = os_;
    payload.arch = arch_;
    payload.signature = effective_signature;
    payload.stack_hint = hint;
    payload.stack_trace = trace;
    payload.breadcrumbs = breadcrumbs_.snapshot();
    payload.user_id = current_user_id();
    payload.steam_id = current_steam_id();
    payload.log = want_log;
    const MatchContext crash_ctx = current_match_context();
    payload.role = crash_ctx.role;
    payload.server_id = crash_ctx.server_id;
    payload.match_id = crash_ctx.match_id;
    payload.session_id = session_id_;
    // Pre-flush buffered events/metrics onto the outbound queue BEFORE enqueuing
    // the crash (mirrors Unity's FlushBatches() in captureException): on a fatal
    // crash the final batch must still be delivered, not lost in memory.
    flush_all_batches();
    enqueue_ingest(crashes_path, build_crash_json(payload), Durability::write_ahead,
                   SidecarKind::crash, want_log, /*log_from_previous=*/false);
    // Final flush in the crash path: the on-disk log must include this crash
    // even if the process dies before the upload (the write-ahead sidecar
    // retries next launch).
    if (want_log) {
        session_log_.flush_now();
    }
    return TOMBSTONE_OK;
}

tombstone_result Client::request_player_logs(const char *target_type, const char *target_value,
                                             const char *reason) {
    if (!initialized_) {
        return TOMBSTONE_ERROR_NOT_INITIALIZED;
    }
    // A server-side operator action (not consent-gated like player capture): queue
    // a log pull. All three fields are required by the server schema.
    if (target_type == nullptr || target_type[0] == '\0' || target_value == nullptr ||
        target_value[0] == '\0' || reason == nullptr || reason[0] == '\0') {
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;
    }
    UploadJob job;
    job.url = endpoint_ + pull_requests_path;
    job.body = build_pull_request_json(target_type, target_value, reason);
    job.durability = Durability::persist_on_failure;  // retried in-session with backoff
    job.no_persist = true;  // a control-plane POST is not a single-item ingest sidecar
    worker_->enqueue(std::move(job));
    return TOMBSTONE_OK;
}

tombstone_result Client::report_bug(const char *category, const char *message, bool attach_log) {
    if (message == nullptr || message[0] == '\0') {
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;
    }
    if (!capture_allowed()) {
        return TOMBSTONE_DISABLED;
    }
    const bool want_log = attach_log && session_log_enabled_ && storage_available_;

    BugReportPayload payload;
    payload.occurred_at_iso = now_iso8601_utc_ms();
    payload.build_version = build_version_;
    payload.os = os_;
    payload.arch = arch_;
    payload.category = category != nullptr ? category : "";
    payload.message = message;
    payload.user_id = current_user_id();
    payload.steam_id = current_steam_id();
    payload.breadcrumbs = breadcrumbs_.snapshot();
    payload.log = want_log;
    const MatchContext bug_ctx = current_match_context();
    payload.role = bug_ctx.role;
    payload.server_id = bug_ctx.server_id;
    payload.match_id = bug_ctx.match_id;
    payload.session_id = session_id_;
    enqueue_ingest(bug_reports_path, build_bug_report_json(payload), Durability::write_ahead,
                   SidecarKind::bug_report, want_log, /*log_from_previous=*/false);
    return TOMBSTONE_OK;
}

}  // namespace tombstone
