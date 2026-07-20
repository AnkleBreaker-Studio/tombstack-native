#ifndef TOMBSTONE_SRC_CLIENT_H
#define TOMBSTONE_SRC_CLIENT_H

#include <tombstone/tombstone.h>

#include "batch.h"
#include "breadcrumb_ring.h"
#include "dedupe.h"
#include "frame_stats.h"
#include "native_crash.h"
#include "payloads.h"
#include "sampler.h"
#include "sdk_log.h"
#include "session_log.h"
#include "session_marker.h"
#include "sidecar_queue.h"
#include "start_gate.h"
#include "worker.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace tombstone {

class Transport;

/**
 * The process-wide SDK instance behind the C ABI. Owns every subsystem and
 * all configuration copies. Lifecycle: construct -> init() once -> capture
 * calls from any thread -> destructor (clean shutdown). The C layer maps one
 * tombstone_init/tombstone_shutdown pair to one Client instance, so
 * use-after-shutdown can never reach a half-dead object.
 */
class Client {
public:
    // Both out-of-line (client.cpp): members hold unique_ptr<Transport> with
    // Transport forward-declared, so the ctor's unwind path and the dtor must
    // be emitted in a TU that sees the complete type.
    Client();
    ~Client();

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
    Client(Client &&) = delete;
    Client &operator=(Client &&) = delete;

    tombstone_result init(const tombstone_options &options);

    tombstone_result set_user(const char *user_id, const char *steam_id);
    tombstone_result set_consent(bool granted);
    /** Release the send gate (one-way idempotent latch): heartbeats begin and
     *  held event/metric batch drains resume. See tombstone_start_session(). */
    tombstone_result start_session();
    tombstone_result set_environment(const char *environment);
    /** REPLACE the per-user metadata map (clamped; NULL/0 clears). Rides
     *  heartbeats via change detection against the last acked map. */
    tombstone_result set_user_metadata(const char *const *keys, const char *const *values,
                                       std::size_t count);
    /** Feed one frame's duration (ms) into the frame-stats accumulator. */
    void report_frame(double frame_ms);
    tombstone_result set_server_info(const char *region, const char *hostname);
    tombstone_result mark_dedicated_server(const char *server_id, const char *region,
                                           const char *hostname);
    tombstone_result set_device(const tombstone_device_t *device);
    tombstone_result set_match_context(const char *server_id, const char *match_id);
    tombstone_result start_match(char *out_match_id, std::size_t out_cap);
    tombstone_result end_match();
    tombstone_result add_breadcrumb(tombstone_level level, const char *message);
    tombstone_result track_event(const char *name, const char *const *keys,
                                 const char *const *values, std::size_t count);
    tombstone_result track_metric(const char *name, double value, const char *unit);
    tombstone_result report_crash(const char *signature, const char *stack_hint,
                                  const char *stack_trace, bool attach_log);
    tombstone_result report_bug(const char *category, const char *message, bool attach_log);
    tombstone_result log_line(tombstone_level level, const char *line);
    tombstone_result request_player_logs(const char *target_type, const char *target_value,
                                         const char *reason);
    void set_sample_rate(const char *name, double rate);
    tombstone_result set_level(const char *level_name);
    tombstone_result diagnostics(tombstone_diagnostics_t *out);
    tombstone_result flush(int timeout_ms);

private:
    // --- lifecycle helpers (client.cpp) ---
    void configure_storage();
    /** v0.8: restore-or-derive the persistent device-derived provisional user
     *  id (the "dev_..." fallback identity). Runs once during init, right
     *  after configure_storage(). */
    void acquire_device_identity();
    /** Read `<data_dir>/identity`; "" unless the content validates. Fail-soft. */
    std::string read_persisted_identity();
    /** Best-effort write of `<data_dir>/identity` (a failure is logged only). */
    void persist_identity(const std::string &id);
    void drain_sidecars();
    void start_session_tracking();
    void report_unclean_shutdown(const SessionMarkerData &previous);
    void report_native_crash(const SessionMarkerData &previous, const NativeCrashDump &dump);
    void heartbeat_loop();
    void stop_heartbeat();
    /** Worker-thread hook: parse a heartbeat ack and fulfil pull requests
     *  targeting this client (consent-gated, fail-soft). */
    void handle_heartbeat_ack(const std::string &response_body);

    // --- capture helpers (client_reports.cpp) ---
    bool capture_allowed() const noexcept { return initialized_ && consent_; }
    void record_breadcrumb(std::string_view level, std::string_view message);
    void enqueue_ingest(const char *path, std::string body, Durability durability,
                        SidecarKind kind, bool request_log, bool log_from_previous);
    /** Worker-thread hook (K1): record the upload round-trip as `tombstone.rtt_ms`. */
    void record_rtt_metric(double ms);

    // --- batching (client.cpp): event/metric envelopes, sent off the worker thread ---
    /** Drain one batch into the queue when a trigger fires (or `force`). `suppress_rtt`
     *  marks the metrics batch so its own upload does not emit a recursive rtt metric. */
    void maybe_flush_batch(Batch &batch, const char *path,
                           std::chrono::steady_clock::time_point now, bool force,
                           bool suppress_rtt);
    /** Worker-thread hook: flush count/age-ready event + metric batches. */
    void drain_ready_batches();
    /** Force-drain both batches (explicit flush / quit / pre-crash). */
    void flush_all_batches();

    /** The EFFECTIVE user id: the integrator-set id, or the device-derived
     *  provisional id when none is set (v0.8: never "" after init). */
    std::string current_user_id() const;
    std::string current_steam_id() const;
    /** The pending one-shot `priorUserId` merge marker ("" = none), stamped on
     *  crash/bug payloads while pending (heartbeats use the beat-build path). */
    std::string current_prior_user_id() const;

    /** Snapshot of the cached multiplayer correlation context (mutex-guarded).
     *  `environment` is stamped on EVERY payload; role/server_id/match_id on all
     *  but ride the heartbeat too. */
    struct MatchContext {
        std::string role;         // "" -> omitted (never an empty-string enum on the wire)
        std::string server_id;    // "" -> omitted
        std::string match_id;     // "" -> omitted
        std::string environment;  // "" -> omitted (server defaults to "production")
    };
    MatchContext current_match_context() const;

    /** Server-lifetime fleet labels, emitted on heartbeats only (mutex-guarded). */
    struct ServerInfo {
        std::string region;    // "" -> omitted
        std::string hostname;  // "" -> omitted
    };
    ServerInfo current_server_info() const;

    /** Snapshot of the caller-supplied device specs (mutex-guarded). */
    DevicePayload current_device() const;

    // diagnostics + storage (order matters: sdk_log_ first, it is referenced)
    SdkLog sdk_log_;
    SessionLog session_log_{sdk_log_};
    SessionMarker marker_{sdk_log_};
    SidecarQueue sidecars_{sdk_log_};
    BreadcrumbRing breadcrumbs_;
    DedupeWindow dedupe_;
    Sampler sampler_;

    // Bounded, drop-oldest batch buffers (declared before worker_ so they
    // outlive the worker thread that drains them; spec section 16).
    Batch event_batch_;
    Batch metric_batch_;

    // transport + delivery (created during init; Worker needs the token)
    std::unique_ptr<Transport> transport_;
    std::unique_ptr<Worker> worker_;

    // configuration copies
    std::string endpoint_;
    std::string token_;
    std::string build_version_;
    std::string os_;
    std::string arch_;
    std::string session_id_;
    std::filesystem::path data_dir_;
    std::chrono::seconds heartbeat_interval_{60};
    bool heartbeats_enabled_{true};
    bool session_log_enabled_{true};
    bool unclean_detection_enabled_{true};
    bool rtt_metric_enabled_{true};
    bool storage_available_{false};

    // K1 diagnostics: steady-clock ns of the last event/metric batch flush (0 = none).
    std::atomic<long long> last_flush_steady_ns_{0};

    // state
    std::atomic<bool> initialized_{false};
    std::atomic<bool> consent_{true};
    // v0.7 send gate (Unity 0.15 parity): until this one-way latch is set, the
    // heartbeat loop sends NOTHING and event/metric batch DRAINS are held
    // (items keep buffering in the bounded batches); crash/bug reports are
    // exempt — their write-ahead path never waits on identity. Armed from
    // options.auto_start_session at init; start_session() sets it forever.
    // Latch semantics live (and are unit-tested) in start_gate.h.
    StartGate start_gate_;
    std::mutex session_tracking_mutex_;
    bool session_tracking_started_{false};
    std::optional<SessionMarkerData> previous_marker_;
    bool had_previous_log_{false};
    bool has_restored_crash_{false};
    // v0.9 native crash handler (experimental, opt-in). `native_crash_enabled_`
    // mirrors the option; `previous_native_dump_` holds a dump recovered from
    // the previous run (picked up at init, reported by start_session_tracking).
    bool native_crash_enabled_{false};
    std::optional<NativeCrashDump> previous_native_dump_;

    mutable std::mutex user_mutex_;
    std::string user_id_;
    std::string steam_id_;
    // v0.8 device identity (Unity 0.16 parity): the persistent device-derived
    // provisional id ("dev_" + 16 hex), acquired at init (identity file, else
    // derived from the machine id salted with the ingest token) so the SDK
    // NEVER sends an anonymous user — current_user_id() falls back to it while
    // user_id_ is empty. When set_user upgrades provisional -> real id, the
    // provisional id becomes the one-shot pending_prior_user_id_, sent as
    // `priorUserId` on heartbeats until one carrying it is acked (crash/bug
    // bodies stamp it too while pending) so the server merges the session's
    // pre-auth rows under the real player. prior_in_flight_ records the value
    // the last-built beat carried; the heartbeat ack commits (clears pending)
    // only when it still equals the pending value — a dropped beat re-carries.
    // All three guarded by user_mutex_ (same discipline as user_id_).
    std::string provisional_user_id_;
    std::string pending_prior_user_id_;
    std::string prior_in_flight_;

    // Per-user metadata (rides HEARTBEATS with change detection, mirroring the
    // Unity SDK M1/M2): a beat carries the `metadata` object only when the
    // current map's canonical JSON differs from the last one a heartbeat ACKED
    // ("{}" = none/clear). The in-flight json/epoch pair is recorded at beat
    // build and committed on the next heartbeat 2xx; the epoch is bumped when
    // set_user changes identity, voiding a stale commit from a pre-change beat.
    // Lock order (never acquire in the reverse direction):
    //   heartbeat_mutex_ -> user_mutex_ / match_mutex_ / metadata_mutex_
    //     (heartbeat_loop holds heartbeat_mutex_ across the whole beat build,
    //     which snapshots user, match context, and metadata under their locks)
    //   user_mutex_ -> metadata_mutex_ (only nested in set_user)
    mutable std::mutex metadata_mutex_;
    UserMetadataEntries user_metadata_;
    std::string metadata_last_acked_json_{"{}"};
    long long metadata_epoch_{0};
    std::string metadata_in_flight_json_;
    long long metadata_in_flight_epoch_{-1};

    // Frame-stats accumulator (tombstone_report_frame), drained onto each beat.
    FrameAccumulator frame_stats_;

    // multiplayer correlation context (role/serverId/matchId stamped on payloads)
    mutable std::mutex match_mutex_;
    std::string role_;
    std::string server_id_;
    std::string match_id_;
    // deployment environment (stamped on every payload) + server-lifetime fleet
    // labels (heartbeat-only) + caller-supplied device specs, all guarded by
    // match_mutex_ (the shared "context" lock, snapshotted once per payload).
    std::string environment_;
    std::string region_;
    std::string hostname_;
    DevicePayload device_;

    // current level / scene context (K2), stored alongside the correlation context
    std::string current_level_;

    // Device-on-heartbeat delivery (mirrors the Unity SDK): the device object rides
    // heartbeats until one carrying it is acked (2xx), then is dropped from beats for
    // the rest of the session. `carry_in_flight_` is raised when a beat carries the
    // device and lowered on the next heartbeat ack, which sets `sent_`. Heartbeats are
    // ephemeral + serialized through one worker queue, so a lost beat simply re-carries.
    std::atomic<bool> device_sent_on_heartbeat_{false};
    std::atomic<bool> device_carry_in_flight_{false};

    // heartbeat timer thread
    std::thread heartbeat_thread_;
    std::mutex heartbeat_mutex_;
    std::condition_variable heartbeat_cv_;
    bool heartbeat_stop_{false};
    // Raised by start_session() so the first beat goes out promptly instead of
    // up to one interval late (guarded by heartbeat_mutex_).
    bool heartbeat_kick_{false};
};

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_CLIENT_H
