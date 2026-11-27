#ifndef TOMBSTONE_SRC_DELIVERY_POLICY_H
#define TOMBSTONE_SRC_DELIVERY_POLICY_H

#include "server_clock.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace tombstone {

/**
 * What one server answer means for delivery (shared with the other Tombstack SDKs):
 *
 * - delivered: 2xx.
 * - retry: no answer, 408, 429, 5xx, or a 401 about the request signature (a wrong
 *   device clock, see server_clock.h). Backoff, raised to a 429/503 Retry-After.
 * - drop: the server refuses this payload itself (400, 404, 413, 422 and any other
 *   4xx not listed here). Retrying cannot help, so it is deleted.
 * - spend_cap: 402, the studio's spending cap is reached. The server refuses
 *   everything until `resumesAt`, so nothing is sent until then; crash and bug
 *   reports are kept and sent once the stop ends.
 * - token_refused: a 401 refusing the SDK token (`invalid_api_key`) or a 403 on an
 *   ingest POST. Nothing more is sent with this token this launch; crash and bug
 *   reports are kept on disk for a later launch.
 */
enum class Settle { delivered, retry, drop, spend_cap, token_refused };

/** Settle an answer from the Tombstack API. `ingest_post`: a signed ingest request
 *  (pull-request calls are not: their 401/403 refuse only that call). Pure. */
Settle settle_api_response(bool transport_error, long status, Unauthorized unauthorized,
                           bool ingest_post) noexcept;

/** Settle an answer from the storage host of a presigned log upload: only
 *  delivered, retry or drop (a 402 or 403 there is not Tombstack's). Pure. */
Settle settle_upload_response(bool transport_error, long status) noexcept;

/** Bounds of a spending-cap stop, as in the Web and Godot SDKs: never shorter than a
 *  minute, and never longer than an hour, so a game still running when the studio
 *  raises its cap resumes within the hour. */
constexpr std::int64_t spend_cap_min_pause_seconds = 60;
constexpr std::int64_t spend_cap_max_pause_seconds = 3600;
/** The stop when the 402 names no usable `resumesAt`. */
constexpr std::int64_t spend_cap_default_pause_seconds = 900;
/** No more of a 402 body than this is read (the body is untrusted). */
constexpr std::size_t spend_cap_max_body_bytes = 64 * 1024;

/** "YYYY-MM-DDTHH:MM:SS[.fraction]Z" (UTC) as Unix seconds; nullopt for any other
 *  shape or an impossible date. Pure. */
std::optional<std::int64_t> parse_iso8601_utc_seconds(std::string_view value) noexcept;

/**
 * Seconds to send nothing after a 402: `resumesAt` from its body minus the server's
 * current time, clamped to [min_pause, 1 h] (min_pause is 60 s outside tests); 15 min
 * when the body names no usable time. Pure.
 */
std::int64_t spend_cap_pause_seconds(std::string_view body, std::int64_t server_now_unix,
                                     std::int64_t min_pause = spend_cap_min_pause_seconds) noexcept;

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_DELIVERY_POLICY_H
