#ifndef TOMBSTONE_SRC_PAYLOADS_H
#define TOMBSTONE_SRC_PAYLOADS_H

#include "breadcrumb_ring.h"

#include <cstddef>
#include <string>
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
}  // namespace limits

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
};

struct HeartbeatPayload {
    std::string session_id;
    std::string occurred_at_iso;
    std::string build_version;
    std::string os;
    std::string arch;
    std::string user_id; // empty -> omitted
};

std::string build_crash_json(const CrashPayload &payload);
std::string build_bug_report_json(const BugReportPayload &payload);
std::string build_event_json(const EventPayload &payload);
std::string build_heartbeat_json(const HeartbeatPayload &payload);

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_PAYLOADS_H
