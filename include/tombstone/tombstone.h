#ifndef TOMBSTONE_TOMBSTONE_H
#define TOMBSTONE_TOMBSTONE_H

/*
 * Tombstone Native SDK — crash & telemetry client for any game engine.
 *
 * Pure C99 ABI: no C++ types cross this boundary and no call ever throws.
 * Every entry point returns a tombstone_result. The SDK is fail-silent by
 * design: internal failures degrade to result codes and (optionally) lines on
 * the user-provided log callback — never to console output or exceptions.
 *
 * The SDK binds to one implicit process-wide client (tombstone_handle is the
 * opaque type reserved for a future multi-instance API). Double-init and
 * use-after-shutdown return result codes; they are never undefined behavior.
 *
 * Crash capture scope (v0.x): this SDK REPORTS crashes you hand it
 * (tombstone_report_crash) and detects unclean shutdowns across launches.
 * It does NOT install signal/SEH handlers or write minidumps — that arrives
 * in Phase 2 via a sentry-native/Crashpad fork (see README roadmap).
 */

#include <stddef.h>

#if defined(_WIN32)
#if defined(TOMBSTONE_STATIC)
#define TOMBSTONE_API
#elif defined(TOMBSTONE_EXPORTS)
#define TOMBSTONE_API __declspec(dllexport)
#else
#define TOMBSTONE_API __declspec(dllimport)
#endif
#else /* !_WIN32 */
#if defined(TOMBSTONE_EXPORTS)
#define TOMBSTONE_API __attribute__((visibility("default")))
#else
#define TOMBSTONE_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque client handle (reserved; the v0 API drives the process-wide instance). */
typedef struct tombstone_handle tombstone_handle;

/** Result of every API call. Negative values are errors; >= 0 is non-fatal. */
typedef enum tombstone_result {
    TOMBSTONE_OK = 0,
    /** Call ignored: consent is off or the feature is disabled. Not an error. */
    TOMBSTONE_DISABLED = 1,
    /** Suppressed as a duplicate (crash dedupe window). Not an error. */
    TOMBSTONE_SUPPRESSED = 2,
    TOMBSTONE_ERROR_INVALID_ARGUMENT = -1,
    TOMBSTONE_ERROR_NOT_INITIALIZED = -2,
    TOMBSTONE_ERROR_ALREADY_INITIALIZED = -3,
    TOMBSTONE_ERROR_IO = -4,
    TOMBSTONE_ERROR_QUEUE_FULL = -5,
    TOMBSTONE_ERROR_TIMEOUT = -6,
    TOMBSTONE_ERROR_INTERNAL = -7
} tombstone_result;

/** Severity for breadcrumbs and session-log lines. */
typedef enum tombstone_level {
    TOMBSTONE_LEVEL_DEBUG = 0,
    TOMBSTONE_LEVEL_INFO = 1,
    TOMBSTONE_LEVEL_WARN = 2,
    TOMBSTONE_LEVEL_ERROR = 3
} tombstone_level;

/**
 * Optional sink for the SDK's own diagnostics (the SDK never writes to
 * stdout/stderr). `level` is a tombstone_level value. Must be thread-safe;
 * may be invoked from SDK background threads. Must not call back into the SDK.
 */
typedef void (*tombstone_log_callback)(int level, const char *message, void *user_data);

/**
 * Initialization options. Zero it, call tombstone_options_init() to apply
 * defaults, then override fields. All strings are copied during init; the
 * caller keeps ownership of the memory it passed.
 */
typedef struct tombstone_options {
    /** Tombstone base URL, e.g. "https://your-tenant.example.com". Required. */
    const char *endpoint;
    /** Per-game SDK token ("tmb_..."). Treat as a build secret. Required. */
    const char *token;
    /** Shipped with every payload (dashboard build filter). Required. */
    const char *build_version;
    /** Writable directory for sidecars, session log, marker. NULL = platform default. */
    const char *data_dir;
    /** Seconds between session heartbeats; clamped to [15, 600]. 0 = default (60). */
    int heartbeat_interval_s;
    /** Nonzero (default) = emit periodic session heartbeats. */
    int enable_heartbeats;
    /** Nonzero (default) = keep a rolling session log, uploadable with reports. */
    int enable_session_log;
    /** Nonzero (default) = detect a hard-killed previous session and report it. */
    int enable_unclean_shutdown_detection;
    /** Nonzero (default) = capture starts immediately. 0 = wait for tombstone_set_consent(1). */
    int consent_granted;
    /** Optional diagnostics sink (see tombstone_log_callback). NULL = silent. */
    tombstone_log_callback log_callback;
    /** Opaque pointer handed back to log_callback. */
    void *log_callback_user_data;
} tombstone_options;

/** Fill `options` with documented defaults. Safe on an uninitialized struct. */
TOMBSTONE_API tombstone_result tombstone_options_init(tombstone_options *options);

/**
 * Initialize the SDK: resolves the data dir, rotates the previous session log,
 * reads the unclean-shutdown marker, drains offline sidecars, and starts the
 * background upload + heartbeat threads. First successful call wins;
 * subsequent calls return TOMBSTONE_ERROR_ALREADY_INITIALIZED.
 */
TOMBSTONE_API tombstone_result tombstone_init(const tombstone_options *options);

/**
 * Clean shutdown: flushes the session log, persists undelivered durable
 * payloads as sidecars, deletes the unclean-shutdown marker, and joins the
 * background threads. Idempotent.
 */
TOMBSTONE_API tombstone_result tombstone_shutdown(void);

/**
 * Attribute subsequent payloads to a player. Values are clamped to the wire
 * contract (user_id 128, steam_id 32 chars). Pass NULL to clear either.
 */
TOMBSTONE_API tombstone_result tombstone_set_user(const char *user_id, const char *steam_id);

/**
 * Toggle capture + upload (store policy / GDPR). While 0, nothing is recorded
 * or sent — breadcrumbs, heartbeats, events, and reports are all ignored.
 */
TOMBSTONE_API tombstone_result tombstone_set_consent(int granted);

/**
 * Tag subsequent telemetry with multiplayer correlation context. `server_id`
 * and `match_id` are stamped (clamped to 128 chars) onto every crash, bug, and
 * event body so server<->match<->session linking is exact. Pass NULL to clear
 * either. Empty/cleared dimensions are omitted from the wire body.
 *
 * `h` is the reserved opaque handle (the v0 API drives the process-wide
 * instance); pass NULL. Fail-soft: a no-op before init.
 */
TOMBSTONE_API void tombstone_set_match_context(tombstone_handle *h, const char *server_id,
                                               const char *match_id);

/**
 * Begin a server-authoritative match: marks role="server", mints a fresh match
 * id, and stamps it on subsequent telemetry until tombstone_end_match(). The
 * 32-char id (plus NUL) is written to `out_match_id`; `out_cap` must be >= 33.
 * Returns TOMBSTONE_ERROR_INVALID_ARGUMENT for a NULL/too-small buffer (the
 * cached context is left unchanged) and TOMBSTONE_ERROR_NOT_INITIALIZED before
 * init. `h` is the reserved opaque handle; pass NULL.
 */
TOMBSTONE_API tombstone_result tombstone_start_match(tombstone_handle *h, char *out_match_id,
                                                     size_t out_cap);

/**
 * End the current match: clears the cached match id so subsequent telemetry is
 * no longer match-tagged (role and server id are retained). `h` is the reserved
 * opaque handle; pass NULL. Fail-soft: a no-op before init.
 */
TOMBSTONE_API void tombstone_end_match(tombstone_handle *h);

/**
 * Add a breadcrumb to the trail attached to future crash and bug reports.
 * Fixed 64-slot ring; oldest entries are overwritten. Message clamped to 512.
 */
TOMBSTONE_API tombstone_result tombstone_add_breadcrumb(tombstone_level level, const char *message);

/**
 * Track a named analytics event with optional flat string attributes
 * (parallel keys[]/values[] arrays of length `count`; both may be NULL when
 * count is 0). Clamped to the wire contract: 32 attributes, 64-char keys,
 * 512-char values, 64-char name. Retried with backoff; persisted offline on
 * final failure.
 */
TOMBSTONE_API tombstone_result tombstone_track_event(const char *name,
                                                     const char *const *keys,
                                                     const char *const *values,
                                                     size_t count);

/**
 * Record a numeric metric sample (e.g. tick rate, RTT, memory use). Batched
 * client-side and flushed on count/age/quit/pre-crash like analytics events;
 * each sample carries its own occurredAtIso plus the cached correlation
 * context (role/serverId/matchId/sessionId). `value` must be finite (NaN /
 * Infinity is dropped). `unit` is an optional short label (e.g. "ms", "fps",
 * "hz"); NULL or "" omits it. `h` is the reserved opaque handle; pass NULL.
 * Fail-soft: a no-op before init or while consent is off.
 */
TOMBSTONE_API void tombstone_track_metric(tombstone_handle *h, const char *name, double value,
                                          const char *unit);

/**
 * Report a crash. `signature` groups occurrences (clamped to 128 chars); pass
 * NULL to derive a stable signature from stack_hint + stack_trace. stack_hint
 * is the one-line summary (clamped to 512; NULL becomes "Crash"); stack_trace
 * is optional (clamped to 8192). attach_log != 0 requests a session-log
 * upload slot. Write-ahead durable: persisted to disk before the first upload
 * attempt. Deduped per signature (<= 1 report/min; repeats return
 * TOMBSTONE_SUPPRESSED and become a counter breadcrumb).
 */
TOMBSTONE_API tombstone_result tombstone_report_crash(const char *signature,
                                                      const char *stack_hint,
                                                      const char *stack_trace,
                                                      int attach_log);

/**
 * Submit a player bug report. `message` is required (clamped to 4000);
 * `category` is optional (clamped to 32). The current breadcrumb trail is
 * attached. attach_log != 0 requests a session-log upload slot. Write-ahead
 * durable like crashes.
 */
TOMBSTONE_API tombstone_result tombstone_report_bug(const char *category,
                                                    const char *message,
                                                    int attach_log);

/**
 * Mirror one game-log line into the rolling session log (512 KB cap with
 * front-trim) and the breadcrumb ring. This is the integration point for an
 * engine's log hook.
 */
TOMBSTONE_API tombstone_result tombstone_log_line(tombstone_level level, const char *line);

/**
 * Server-side: queue a log pull for a player / session / match / whole server.
 * `target_type` is one of "userId" | "sessionId" | "matchId" | "server"
 * (clamped to 32 chars); `target_value` is the corresponding id (clamped to
 * 128); `reason` is the audit note (clamped to 280). Requires a WRITE-scoped
 * server token — an ingest-only client token is rejected server-side (the 403
 * is logged via the diagnostics callback, never fatal). The pull is queued; the
 * targeted client(s) upload on their next heartbeat (consent-gated). Fail-silent:
 * a no-op before init or on a NULL/empty argument. `h` is the reserved opaque
 * handle; pass NULL.
 */
TOMBSTONE_API void tombstone_request_player_logs(tombstone_handle *h, const char *target_type,
                                                 const char *target_value, const char *reason);

/**
 * Server-side convenience: auto-pull a player's logs after an anomalous
 * disconnect. Equivalent to tombstone_request_player_logs(h, "userId", user_id,
 * reason); a NULL/empty `reason` defaults to "anomalous disconnect". `h` is the
 * reserved opaque handle; pass NULL. Fail-silent.
 */
TOMBSTONE_API void tombstone_on_anomalous_disconnect(tombstone_handle *h, const char *user_id,
                                                     const char *reason);

/**
 * Block until the outbound queue is drained (delivered, persisted offline, or
 * dropped as poison) or `timeout_ms` elapses. Returns TOMBSTONE_ERROR_TIMEOUT
 * when work remains. timeout_ms <= 0 means "do not wait, just report".
 */
TOMBSTONE_API tombstone_result tombstone_flush(int timeout_ms);

/** SDK semantic version string, e.g. "0.1.0". Never NULL. */
TOMBSTONE_API const char *tombstone_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TOMBSTONE_TOMBSTONE_H */
