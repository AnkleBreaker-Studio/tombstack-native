#include "payloads.h"

#include "json_writer.h"

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

}  // namespace

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
    json.end_object();
    return json.str();
}

}  // namespace tombstone
