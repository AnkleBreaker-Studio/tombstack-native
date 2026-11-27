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
 * - drop: the server rejects this payload itself as malformed or too large (400,
 *   413, 422). Retrying cannot help, so it is deleted.
 * - spend_cap: 402, the studio's spending cap is reached. The server refuses
 *   everything until `resumesAt`, so nothing is sent until then; crash and bug
 *   reports are kept and sent once the stop ends.
 * - stop_launch: any other refusal of an ingest POST — a refused SDK token (401
 *   `invalid_api_key`), a 403, a 404 (usually a wrong endpoint), a redirect.
 *   Nothing more is sent this launch; crash and bug reports are kept on disk for a
 *   later launch, bounded by the queue limits and the 90-day expiry.
 *
 * A pull-request call (log pulls) is not an ingest POST: the server answers a stale or
 * mistargeted one with 403 or 410, which refuses that call alone, so any refusal of
 * one other than 402 drops just that call.
 */
enum class Settle { delivered, retry, drop, spend_cap, stop_launch };

/** Settle an answer from the Tombstack API. `ingest_post`: a signed ingest request,
 *  as opposed to a pull-request call. Pure. */
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
