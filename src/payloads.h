#ifndef TOMBSTONE_SRC_PAYLOADS_H
#define TOMBSTONE_SRC_PAYLOADS_H

#include "breadcrumb_ring.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tombstone {

/**
 * Wire-body builders for the four ingest endpoints. Each mirrors its Zod
 * schema exactly: fields appear in schema order, absent optionals are OMITTED
 * (never serialized as ""), strings are clamped to the schema maxima
 * (UTF-8-safe), and `log` is always an explicit boolean on crash/bug bodies
 * (`false` means "no log" per the server contract).
 */

/** Schema maxima (the per-endpoint schema files under src/lib in the Tombstone monorepo). */
namespace limits {
constexpr std::size_t build_version = 64;
constexpr std::size_t signature = 128;
constexpr std::size_t stack_hint = 512;
constexpr std::size_t stack_trace = 8192;
constexpr std::size_t user_id = 128;
constexpr std::size_t steam_id = 32;
constexpr std::size_t bug_category = 32;
constexpr std::size_t bug_message = 4000;
constexpr std::size_t event_name = 64;
constexpr std::size_t event_attributes = 32;
constexpr std::size_t event_attribute_key = 64;
constexpr std::size_t event_attribute_value = 512;
constexpr std::size_t session_id = 64;
constexpr std::size_t role = 16;       // enum "client"/"server"; never emitted empty
constexpr std::size_t server_id = 128;
constexpr std::size_t match_id = 128;
constexpr std::size_t metric_name = 64;
constexpr std::size_t metric_unit = 16; // short label e.g. "ms"/"fps"/"hz"
constexpr std::size_t pull_target_type = 32;  // enum "userId"/"sessionId"/"matchId"/"server"
constexpr std::size_t pull_target_value = 128;
constexpr std::size_t pull_reason = 280;      // server MAX_PULL_REASON
constexpr std::size_t pull_nonce = 256;       // server pullFulfillSchema nonce max
constexpr std::size_t environment = 64;       // deployment environment ("production" server default)
constexpr std::size_t region = 64;            // fleet label, heartbeats only
constexpr std::size_t hostname = 255;         // fleet label, heartbeats only
// per-user metadata map (server user-metadata.ts MAX_USER_METADATA_*), heartbeats only
constexpr std::size_t user_metadata_keys = 16;
constexpr std::size_t user_metadata_key = 64;
constexpr std::size_t user_metadata_value = 512;
// device object maxima (server deviceSchema; crash/bug/heartbeat bodies)
constexpr std::size_t device_model = 256;
constexpr std::size_t device_type = 32;
constexpr std::size_t device_os = 256;
constexpr std::size_t device_os_family = 48;
constexpr std::size_t device_cpu = 256;
constexpr std::size_t device_gpu = 256;
constexpr std::size_t device_gpu_vendor = 256;
constexpr std::size_t device_gpu_version = 256;
constexpr std::size_t device_gpu_api = 64;
constexpr std::size_t device_screen = 32;     // "WxH"
constexpr std::size_t device_orientation = 32;
constexpr std::size_t device_language = 48;
constexpr std::size_t device_engine = 48;
constexpr std::size_t device_scripting_backend = 32;
constexpr std::size_t device_platform = 48;
}  // namespace limits

/**
 * Device specs, mirrored from the caller (the SDK reports what it is handed,
 * it never probes hardware). Every field is optional: an empty string / 0
 * means "unset" and is OMITTED from the wire body; a default-constructed
 * struct emits no `device` object at all. Emitted on crash, bug-report, and
 * heartbeat bodies (the latter only until one heartbeat is acked).
 */
struct DevicePayload {
    std::string model;             // empty -> omitted
    std::string type;              // empty -> omitted
    std::string os;                // empty -> omitted
    std::string os_family;         // empty -> omitted
    std::string cpu;               // empty -> omitted
    int cpu_count{0};              // <= 0 -> omitted
    int ram_mb{0};                 // <= 0 -> omitted
    std::string gpu;               // empty -> omitted
    std::string gpu_vendor;        // empty -> omitted
    std::string gpu_version;       // empty -> omitted
    std::string gpu_api;           // empty -> omitted
    int vram_mb{0};                // <= 0 -> omitted
    std::string screen;            // "WxH"; empty -> omitted
    double screen_dpi{0.0};        // <= 0 / non-finite -> omitted
    double refresh_rate{0.0};      // <= 0 / non-finite -> omitted
    std::string orientation;       // empty -> omitted
    bool fullscreen{false};        // false -> omitted (the server cleans 0/false anyway)
    std::string language;          // empty -> omitted
    std::string engine;            // empty -> omitted
    std::string scripting_backend; // empty -> omitted
    std::string platform;          // empty -> omitted
};

/** True when at least one device field is set (i.e. a `device` object would be emitted). */
bool device_has_content(const DevicePayload &device) noexcept;

/** Per-user metadata entries in insertion order (a flat string -> string map). */
using UserMetadataEntries = std::vector<std::pair<std::string, std::string>>;

/**
 * Clamp caller-supplied metadata to the wire contract (server user-metadata.ts):
 * at most user_metadata_keys pairs (first-seen keys win, matching the server's
 * cleanUserMetadata), keys/values UTF-8-safe truncated to user_metadata_key /
 * user_metadata_value, NULL/empty keys and values skipped (an empty value means
 * "unset" server-side), and a duplicate key updates in place (last value wins).
 * NULL arrays / count 0 return an empty map (= a clear).
 */
UserMetadataEntries clamp_user_metadata(const char *const *keys, const char *const *values,
                                        std::size_t count);

/**
 * Serialize a metadata map to its canonical `metadata` JSON object — `{}` when
 * empty (the explicit "clear" the server deletes the stored record on). The
 * client compares these strings for heartbeat change detection AND splices the
 * same string onto the beat, so comparison and wire bytes can never diverge.
 */
std::string build_user_metadata_json(const UserMetadataEntries &entries);

struct CrashPayload {
    std::string occurred_at_iso;
    std::string build_version;
    std::string os;
    std::string arch;
    std::string signature;
    std::string stack_hint;
    std::string stack_trace;             // empty -> omitted
    std::vector<Breadcrumb> breadcrumbs; // empty -> omitted
    std::string user_id;                 // empty -> omitted
    std::string steam_id;                // empty -> omitted
    bool log{false};                     // always serialized
    std::string role;                    // "client"/"server"; empty -> omitted
    std::string server_id;               // empty -> omitted
    std::string match_id;                // empty -> omitted
    std::string session_id;              // empty -> omitted
    std::string environment;             // empty -> omitted (server defaults to "production")
    DevicePayload device;                // all-unset -> `device` object omitted
};

struct BugReportPayload {
    std::string occurred_at_iso;
    std::string build_version;
    std::string os;
    std::string arch;
    std::string category;                // empty -> omitted
    std::string message;                 // required (min 1)
    std::string user_id;                 // empty -> omitted
    std::string steam_id;                // empty -> omitted
    std::vector<Breadcrumb> breadcrumbs; // empty -> omitted
    bool log{false};                     // always serialized
    std::string role;                    // "client"/"server"; empty -> omitted
    std::string server_id;               // empty -> omitted
    std::string match_id;                // empty -> omitted
    std::string session_id;              // empty -> omitted
    std::string environment;             // empty -> omitted (server defaults to "production")
    DevicePayload device;                // all-unset -> `device` object omitted
};

struct EventPayload {
    std::string occurred_at_iso;
    std::string build_version;
    std::string os;
    std::string arch;
    std::string name;    // required (min 1)
    std::string user_id; // empty -> omitted
    /** Flat string attributes in insertion order; empty -> omitted. */
    std::vector<std::pair<std::string, std::string>> attributes;
    std::string role;       // "client"/"server"; empty -> omitted
    std::string server_id;  // empty -> omitted
    std::string match_id;   // empty -> omitted
    std::string session_id; // empty -> omitted
    std::string environment; // empty -> omitted (server defaults to "production")
};

struct MetricPayload {
    std::string name;        // required (min 1)
    double value{0.0};       // finite (guarded at the API boundary); a JSON number
    std::string unit;        // empty -> omitted
    std::string occurred_at_iso;
    std::string build_version;
    std::string os;
    std::string arch;
    std::string user_id;     // empty -> omitted
    std::string role;        // "client"/"server"; empty -> omitted
    std::string server_id;   // empty -> omitted
    std::string match_id;    // empty -> omitted
    std::string session_id;  // empty -> omitted
    std::string environment; // empty -> omitted (server defaults to "production")
};

struct HeartbeatPayload {
    std::string session_id;
    std::string occurred_at_iso;
    std::string build_version;
    std::string os;
    std::string arch;
    std::string user_id;   // empty -> omitted
    /** Pre-serialized `metadata` object (build_user_metadata_json), spliced
     *  verbatim. "{}" is the explicit clear; empty string -> field omitted
     *  (carried only when it differs from the last acked map). */
    std::string metadata_json;
    // Per-interval frame statistics (heartbeat-schema.ts optionals), spliced
    // only when has_frame_stats (>= 1 frame sampled). Values pre-clamped by
    // the FrameAccumulator; hitchCount/worstFrameMs are JSON integers.
    bool has_frame_stats{false};
    double fps_avg{0.0};
    double slow_frame_pct{0.0};
    long long hitch_count{0};
    long long worst_frame_ms{0};
    std::string role;      // "client"/"server"; empty -> omitted
    std::string server_id; // empty -> omitted
    std::string match_id;  // empty -> omitted
    std::string region;    // fleet label; empty -> omitted
    std::string hostname;  // fleet label; empty -> omitted
    std::string environment; // empty -> omitted (server defaults to "production")
    DevicePayload device;  // carried until one heartbeat is acked; all-unset -> omitted
};

std::string build_crash_json(const CrashPayload &payload);
std::string build_bug_report_json(const BugReportPayload &payload);
std::string build_event_json(const EventPayload &payload);
std::string build_heartbeat_json(const HeartbeatPayload &payload);

/** One metric item body (mirrors the server metricSchema; `value` is a JSON
 *  number, empty optionals omitted). Used both for the single-metric endpoint
 *  and as an item inside a metrics batch. */
std::string build_metric_json(const MetricPayload &payload);

/** Transport envelope for a batch POST: `{"sentAtIso":...,"items":[<item>,...]}`.
 *  `items` are already-serialized object bodies, spliced in verbatim. */
std::string build_batch_envelope(const std::string &sent_at_iso,
                                 const std::vector<std::string> &items);

/**
 * Body for `POST /api/v1/pull-requests` (the server-side log-pull trigger).
 * Mirrors the server `pullRequestCreateSchema` exactly: `{ targetType,
 * targetValue, reason }`, all required, clamped to the schema maxima. `targetType`
 * is one of "userId"/"sessionId"/"matchId"/"server".
 */
std::string build_pull_request_json(std::string_view target_type, std::string_view target_value,
                                    std::string_view reason);

/**
 * Body for `POST /api/v1/pull-requests/{requestId}/fulfill` — the client's asserted
 * correlation identity plus the single-use fulfilment nonce (S1). Mirrors the server
 * `pullFulfillSchema`, field order userId/sessionId/matchId/serverId/nonce/nonceExpiry.
 * The correlation ids are optional and an empty value is OMITTED (never serialized as
 * ""). `nonce` (the `fulfillNonce` echoed back from the heartbeat ack) and `nonceExpiry`
 * are emitted as a pair only when `nonce` is non-empty: an older server mints no nonce,
 * so the SDK falls back to the pre-S1 body shape rather than send empty credentials.
 */
std::string build_pull_fulfill_json(const std::string &user_id, const std::string &session_id,
                                    const std::string &match_id, const std::string &server_id,
                                    const std::string &nonce, long long nonce_expiry);

/**
 * Pure: does a queued pull request target THIS client? Mirror of the server's
 * `heartbeatMatchesRequest` so a client uploads only when genuinely targeted (and
 * only ever its OWN log). `server` matches the client's serverId (fan-out to
 * everyone on the box); userId/sessionId/matchId match the client's own values.
 * An empty target value, an unknown target type, or an empty asserted value never
 * matches (an anonymous client is not pulled by a userId request).
 */
bool pull_request_targets_client(std::string_view target_type, std::string_view target_value,
                                 std::string_view user_id, std::string_view session_id,
                                 std::string_view match_id, std::string_view server_id);

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_PAYLOADS_H
