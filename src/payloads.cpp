#include "payloads.h"

#include "json_writer.h"

#include <cmath>

namespace tombstone {

namespace {

void clamped_field(JsonWriter &json, std::string_view name, std::string_view value,
                   std::size_t max_bytes) {
    json.string_field(name, utf8_safe_truncate(value, max_bytes));
}

void optional_field(JsonWriter &json, std::string_view name, const std::string &value,
                    std::size_t max_bytes) {
    if (!value.empty()) {
        clamped_field(json, name, value, max_bytes);
    }
}

void breadcrumbs_field(JsonWriter &json, const std::vector<Breadcrumb> &breadcrumbs) {
    if (breadcrumbs.empty()) {
        return;
    }
    json.begin_array("breadcrumbs");
    for (const Breadcrumb &crumb : breadcrumbs) {
        json.begin_object();
        if (!crumb.ts_iso.empty()) {
            json.string_field("tsIso", crumb.ts_iso);
        }
        clamped_field(json, "level", crumb.level, BreadcrumbRing::max_level_bytes);
        clamped_field(json, "message", crumb.message, BreadcrumbRing::max_message_bytes);
        json.end_object();
    }
    json.end_array();
}

void common_context(JsonWriter &json, const std::string &occurred_at_iso,
                    const std::string &build_version, const std::string &os,
                    const std::string &arch) {
    json.string_field("occurredAtIso", occurred_at_iso);
    clamped_field(json, "buildVersion", build_version, limits::build_version);
    json.string_field("os", os);
    json.string_field("arch", arch);
}

/** Multiplayer correlation spine (role/serverId/matchId/sessionId). Each is an
 *  optional dimension: omitted when empty (never an empty-string enum), so a
 *  plain client body is byte-identical to the pre-correlation wire shape. */
void correlation_fields(JsonWriter &json, const std::string &role, const std::string &server_id,
                        const std::string &match_id, const std::string &session_id) {
    optional_field(json, "role", role, limits::role);
    optional_field(json, "serverId", server_id, limits::server_id);
    optional_field(json, "matchId", match_id, limits::match_id);
    optional_field(json, "sessionId", session_id, limits::session_id);
}

/** Emit an integer device dimension only when it carries information (> 0). */
void optional_int_field(JsonWriter &json, std::string_view name, int value) {
    if (value > 0) {
        json.int_field(name, static_cast<long long>(value));
    }
}

/** Emit a numeric device dimension only when finite and > 0 (0 means "unset"). */
void optional_number_field(JsonWriter &json, std::string_view name, double value) {
    if (std::isfinite(value) && value > 0.0) {
        json.number_field(name, value);
    }
}

/** The optional `device` object (crash/bug/heartbeat bodies). Fields appear in
 *  server-schema order; every dimension is omitted when unset (empty string /
 *  0 / false — the server cleans those anyway), and the whole object is
 *  omitted when nothing is set, keeping a device-less body byte-identical to
 *  the pre-device wire shape. */
void device_object(JsonWriter &json, const DevicePayload &device) {
    if (!device_has_content(device)) {
        return;
    }
    json.begin_object("device");
    optional_field(json, "model", device.model, limits::device_model);
    optional_field(json, "type", device.type, limits::device_type);
    optional_field(json, "os", device.os, limits::device_os);
    optional_field(json, "osFamily", device.os_family, limits::device_os_family);
    optional_field(json, "cpu", device.cpu, limits::device_cpu);
    optional_int_field(json, "cpuCount", device.cpu_count);
    optional_int_field(json, "ramMB", device.ram_mb);
    optional_field(json, "gpu", device.gpu, limits::device_gpu);
    optional_field(json, "gpuVendor", device.gpu_vendor, limits::device_gpu_vendor);
    optional_field(json, "gpuVersion", device.gpu_version, limits::device_gpu_version);
    optional_field(json, "gpuApi", device.gpu_api, limits::device_gpu_api);
    optional_int_field(json, "vramMB", device.vram_mb);
    optional_field(json, "screen", device.screen, limits::device_screen);
    optional_number_field(json, "screenDpi", device.screen_dpi);
    optional_number_field(json, "refreshRate", device.refresh_rate);
    optional_field(json, "orientation", device.orientation, limits::device_orientation);
    if (device.fullscreen) {
        json.bool_field("fullscreen", true);
    }
    optional_field(json, "language", device.language, limits::device_language);
    optional_field(json, "engine", device.engine, limits::device_engine);
    optional_field(json, "scriptingBackend", device.scripting_backend,
                   limits::device_scripting_backend);
    optional_field(json, "platform", device.platform, limits::device_platform);
    json.end_object();
}

}  // namespace

UserMetadataEntries clamp_user_metadata(const char *const *keys, const char *const *values,
                                        std::size_t count) {
    UserMetadataEntries out;
    if (keys == nullptr || values == nullptr || count == 0) {
        return out;  // a clear
    }
    out.reserve(count < limits::user_metadata_keys ? count : limits::user_metadata_keys);
    for (std::size_t i = 0; i < count; ++i) {
        if (keys[i] == nullptr || keys[i][0] == '\0') {
            continue;  // unusable key
        }
        if (values[i] == nullptr || values[i][0] == '\0') {
            continue;  // empty value = "unset" server-side; never serialized
        }
        std::string key{utf8_safe_truncate(keys[i], limits::user_metadata_key)};
        std::string value{utf8_safe_truncate(values[i], limits::user_metadata_value)};
        bool replaced = false;
        for (auto &entry : out) {
            if (entry.first == key) {
                entry.second = std::move(value);  // duplicate key: last value wins
                replaced = true;
                break;
            }
        }
        if (replaced) {
            continue;
        }
        if (out.size() >= limits::user_metadata_keys) {
            break;  // cap: first-seen keys win (mirrors the server cleanUserMetadata)
        }
        out.emplace_back(std::move(key), std::move(value));
    }
    return out;
}

std::string build_user_metadata_json(const UserMetadataEntries &entries) {
    JsonWriter json;
    json.begin_object();
    for (const auto &[key, value] : entries) {
        json.string_field(utf8_safe_truncate(key, limits::user_metadata_key),
                          utf8_safe_truncate(value, limits::user_metadata_value));
    }
    json.end_object();
    return json.str();
}

bool device_has_content(const DevicePayload &device) noexcept {
    return !device.model.empty() || !device.type.empty() || !device.os.empty() ||
           !device.os_family.empty() || !device.cpu.empty() || device.cpu_count > 0 ||
           device.ram_mb > 0 || !device.gpu.empty() || !device.gpu_vendor.empty() ||
           !device.gpu_version.empty() || !device.gpu_api.empty() || device.vram_mb > 0 ||
           !device.screen.empty() ||
           (std::isfinite(device.screen_dpi) && device.screen_dpi > 0.0) ||
           (std::isfinite(device.refresh_rate) && device.refresh_rate > 0.0) ||
           !device.orientation.empty() || device.fullscreen || !device.language.empty() ||
           !device.engine.empty() || !device.scripting_backend.empty() ||
           !device.platform.empty();
}

std::string build_crash_json(const CrashPayload &payload) {
    JsonWriter json;
    json.begin_object();
    common_context(json, payload.occurred_at_iso, payload.build_version, payload.os, payload.arch);
    clamped_field(json, "signature", payload.signature, limits::signature);
    clamped_field(json, "stackHint", payload.stack_hint, limits::stack_hint);
    optional_field(json, "stackTrace", payload.stack_trace, limits::stack_trace);
    breadcrumbs_field(json, payload.breadcrumbs);
    optional_field(json, "userId", payload.user_id, limits::user_id);
    optional_field(json, "steamId", payload.steam_id, limits::steam_id);
    json.bool_field("log", payload.log);
    correlation_fields(json, payload.role, payload.server_id, payload.match_id, payload.session_id);
    optional_field(json, "environment", payload.environment, limits::environment);
    device_object(json, payload.device);
    json.end_object();
    return json.str();
}

std::string build_bug_report_json(const BugReportPayload &payload) {
    JsonWriter json;
    json.begin_object();
    common_context(json, payload.occurred_at_iso, payload.build_version, payload.os, payload.arch);
    optional_field(json, "category", payload.category, limits::bug_category);
    clamped_field(json, "message", payload.message, limits::bug_message);
    optional_field(json, "userId", payload.user_id, limits::user_id);
    optional_field(json, "steamId", payload.steam_id, limits::steam_id);
    breadcrumbs_field(json, payload.breadcrumbs);
    json.bool_field("log", payload.log);
    correlation_fields(json, payload.role, payload.server_id, payload.match_id, payload.session_id);
    optional_field(json, "environment", payload.environment, limits::environment);
    device_object(json, payload.device);
    json.end_object();
    return json.str();
}

std::string build_event_json(const EventPayload &payload) {
    JsonWriter json;
    json.begin_object();
    common_context(json, payload.occurred_at_iso, payload.build_version, payload.os, payload.arch);
    clamped_field(json, "name", payload.name, limits::event_name);
    optional_field(json, "userId", payload.user_id, limits::user_id);
    if (!payload.attributes.empty()) {
        json.begin_object("attributes");
        std::size_t written = 0;
        for (const auto &[key, value] : payload.attributes) {
            if (written >= limits::event_attributes) {
                break;
            }
            if (key.empty()) {
                continue;
            }
            json.string_field(utf8_safe_truncate(key, limits::event_attribute_key),
                              utf8_safe_truncate(value, limits::event_attribute_value));
            ++written;
        }
        json.end_object();
    }
    correlation_fields(json, payload.role, payload.server_id, payload.match_id, payload.session_id);
    optional_field(json, "environment", payload.environment, limits::environment);
    json.end_object();
    return json.str();
}

std::string build_metric_json(const MetricPayload &payload) {
    JsonWriter json;
    json.begin_object();
    clamped_field(json, "name", payload.name, limits::metric_name);
    json.number_field("value", payload.value);
    optional_field(json, "unit", payload.unit, limits::metric_unit);
    json.string_field("occurredAtIso", payload.occurred_at_iso);
    clamped_field(json, "buildVersion", payload.build_version, limits::build_version);
    json.string_field("os", payload.os);
    json.string_field("arch", payload.arch);
    optional_field(json, "userId", payload.user_id, limits::user_id);
    correlation_fields(json, payload.role, payload.server_id, payload.match_id, payload.session_id);
    optional_field(json, "environment", payload.environment, limits::environment);
    json.end_object();
    return json.str();
}

std::string build_batch_envelope(const std::string &sent_at_iso,
                                 const std::vector<std::string> &items) {
    JsonWriter json;
    json.begin_object();
    json.string_field("sentAtIso", sent_at_iso);
    json.begin_array("items");
    for (const std::string &item : items) {
        json.raw_element(item);
    }
    json.end_array();
    json.end_object();
    return json.str();
}

std::string build_heartbeat_json(const HeartbeatPayload &payload) {
    JsonWriter json;
    json.begin_object();
    clamped_field(json, "sessionId", payload.session_id, limits::session_id);
    json.string_field("occurredAtIso", payload.occurred_at_iso);
    clamped_field(json, "buildVersion", payload.build_version, limits::build_version);
    json.string_field("os", payload.os);
    json.string_field("arch", payload.arch);
    optional_field(json, "userId", payload.user_id, limits::user_id);
    // Per-user custom metadata (tombstone_set_user_metadata): a pre-serialized
    // JSON object spliced verbatim (the client compares the same string for
    // change detection, so wire bytes and baseline can never diverge). Carried
    // only when the map changed since the last acked beat; "{}" is the explicit
    // clear the server deletes the stored record on. Empty -> omitted.
    if (!payload.metadata_json.empty()) {
        json.raw_field("metadata", payload.metadata_json);
    }
    // Per-interval frame statistics (heartbeat-schema.ts optionals): present
    // only when >= 1 frame was sampled this interval. hitchCount/worstFrameMs
    // are JSON integers; all values pre-clamped by the FrameAccumulator.
    if (payload.has_frame_stats) {
        json.number_field("fpsAvg", payload.fps_avg);
        json.number_field("slowFramePct", payload.slow_frame_pct);
        json.int_field("hitchCount", payload.hitch_count);
        json.int_field("worstFrameMs", payload.worst_frame_ms);
    }
    // serverContextFields: role/serverId/matchId (the server registers Fleet
    // servers and selects pull requests from these). sessionId is already a
    // required top-level field above, so it is not part of this subset. Each is
    // omitted when empty (never an empty-string enum), matching cleanOptionalId.
    optional_field(json, "role", payload.role, limits::role);
    optional_field(json, "serverId", payload.server_id, limits::server_id);
    optional_field(json, "matchId", payload.match_id, limits::match_id);
    // Fleet labels + deployment environment (heartbeat-only extras, matching the
    // Unity SDK): region/hostname let the dashboard group dedicated servers;
    // environment separates prod/staging/dev traffic (server default "production").
    optional_field(json, "region", payload.region, limits::region);
    optional_field(json, "hostname", payload.hostname, limits::hostname);
    optional_field(json, "environment", payload.environment, limits::environment);
    // Device specs ride heartbeats only until one beat is acked (the client
    // clears them from subsequent beats; a lost beat re-carries them).
    device_object(json, payload.device);
    json.end_object();
    return json.str();
}

std::string build_pull_request_json(std::string_view target_type, std::string_view target_value,
                                    std::string_view reason) {
    JsonWriter json;
    json.begin_object();
    clamped_field(json, "targetType", target_type, limits::pull_target_type);
    clamped_field(json, "targetValue", target_value, limits::pull_target_value);
    clamped_field(json, "reason", reason, limits::pull_reason);
    json.end_object();
    return json.str();
}

std::string build_pull_fulfill_json(const std::string &user_id, const std::string &session_id,
                                    const std::string &match_id, const std::string &server_id,
                                    const std::string &nonce, long long nonce_expiry) {
    JsonWriter json;
    json.begin_object();
    optional_field(json, "userId", user_id, limits::user_id);
    optional_field(json, "sessionId", session_id, limits::session_id);
    optional_field(json, "matchId", match_id, limits::match_id);
    optional_field(json, "serverId", server_id, limits::server_id);
    // S1: present the fulfilment nonce minted for this request (with its expiry) so the
    // server can authenticate the honouring client. Emitted as a pair only when present
    // — an older server mints none, so the body stays the pre-S1 shape (fail-soft).
    if (!nonce.empty()) {
        clamped_field(json, "nonce", nonce, limits::pull_nonce);
        json.int_field("nonceExpiry", nonce_expiry);
    }
    json.end_object();
    return json.str();
}

bool pull_request_targets_client(std::string_view target_type, std::string_view target_value,
                                 std::string_view user_id, std::string_view session_id,
                                 std::string_view match_id, std::string_view server_id) {
    if (target_value.empty()) {
        return false;
    }
    if (target_type == "userId") {
        return !user_id.empty() && user_id == target_value;
    }
    if (target_type == "sessionId") {
        return !session_id.empty() && session_id == target_value;
    }
    if (target_type == "matchId") {
        return !match_id.empty() && match_id == target_value;
    }
    if (target_type == "server") {
        return !server_id.empty() && server_id == target_value;
    }
    return false;
}

}  // namespace tombstone
