#ifndef TOMBSTONE_SRC_JSON_SCAN_H
#define TOMBSTONE_SRC_JSON_SCAN_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

/** First integer value for `key`. nullopt when absent or not an integer token.
 *  Reads an optional sign then digits (bounded); a fractional/exponent value is
 *  rejected. Sufficient for the small Unix-epoch numbers the SDK reads back. */
std::optional<long long> find_int_field(std::string_view json, std::string_view key);

/**
 * Presigned session-log PUT URL from a crash/bug ingest response:
 * locates the `"logUpload"` object, then its `"url"`. nullopt when the server
 * granted no slot (the `logUpload` field is optional in the envelope).
 */
std::optional<std::string> find_log_upload_url(std::string_view response_body);

/** One pending log-pull request as read from the heartbeat ack command channel. */
struct PendingPullRequest {
    std::string request_id;
    std::string target_type;  // userId | sessionId | matchId | server
    std::string target_value;
    std::string fulfill_nonce;   // single-use nonce minted for this request (S1); "" on older servers
    long long nonce_expiry{0};   // Unix-epoch expiry of fulfill_nonce; echoed back verbatim
};

/**
 * Pull requests targeting this client, read from the heartbeat 202 ack's
 * `data.pendingRequests` array. Each element's `requestId` / `targetType` /
 * `targetValue` strings are extracted; the result is bounded (a runaway list is
 * truncated). Returns empty when the array is absent, empty, or malformed —
 * never throws.
 */
std::vector<PendingPullRequest> find_pending_requests(std::string_view response_body);

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_JSON_SCAN_H
