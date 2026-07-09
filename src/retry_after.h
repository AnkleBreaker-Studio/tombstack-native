#ifndef TOMBSTONE_SRC_RETRY_AFTER_H
#define TOMBSTONE_SRC_RETRY_AFTER_H

#include <chrono>
#include <string_view>

namespace tombstone {

/** Upper bound honored for a server-provided Retry-After, in seconds. A
 *  hostile/buggy server must not park the retry queue for hours. */
constexpr long max_retry_after_seconds = 300;

/**
 * Parse the seconds form of an HTTP Retry-After header value (e.g. "120").
 * Surrounding whitespace is tolerated. Returns -1 for an absent/empty value,
 * the HTTP-date form, a negative sign, or anything else malformed — the
 * caller then falls back to its own exponential backoff.
 */
long parse_retry_after_seconds(std::string_view value) noexcept;

/**
 * Delay before retry attempt `attempt` (1-based): the SDK's exponential
 * backoff (2 s, 4 s, 8 s, 16 s), raised to a server-provided Retry-After when
 * that is larger — max(backoff, retry_after) — and capped at
 * max_retry_after_seconds. `retry_after_seconds` < 0 means "no header".
 */
std::chrono::seconds retry_delay(int attempt, long retry_after_seconds) noexcept;

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_RETRY_AFTER_H
