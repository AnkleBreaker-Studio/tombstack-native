#ifndef TOMBSTONE_SRC_JSON_SCAN_H
#define TOMBSTONE_SRC_JSON_SCAN_H

#include <optional>
#include <string>
#include <string_view>

namespace tombstone {

/**
 * Minimal forward scanners for the few JSON values the SDK reads back:
 * the ingest response's presigned log-upload URL, the session marker's
 * fields, and the `"log":true` flag inside restored sidecar bodies.
 *
 * These are NOT general JSON parsers — they find the first occurrence of
 * `"key"` followed by `:` and read the scalar after it, handling standard
 * string escapes. That is sufficient (and deliberately conservative) for the
 * small, server- or SDK-authored documents involved.
 */

/** First string value for `key`, unescaped. nullopt when absent or not a string. */
std::optional<std::string> find_string_field(std::string_view json, std::string_view key);

/** First boolean value for `key`. nullopt when absent or not true/false. */
std::optional<bool> find_bool_field(std::string_view json, std::string_view key);

/**
 * Presigned session-log PUT URL from a crash/bug ingest response:
 * locates the `"logUpload"` object, then its `"url"`. nullopt when the server
 * granted no slot (the `logUpload` field is optional in the envelope).
 */
std::optional<std::string> find_log_upload_url(std::string_view response_body);

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_JSON_SCAN_H
