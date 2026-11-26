#ifndef TOMBSTONE_SRC_SERVER_CLOCK_H
#define TOMBSTONE_SRC_SERVER_CLOCK_H

#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>

namespace tombstone {

/**
 * Parse an IMF-fixdate (`Sun, 06 Nov 1994 08:49:37 GMT`, the only form a
 * current server sends in `Date`) into Unix seconds. Surrounding whitespace and
 * CR/LF are ignored. nullopt for any other shape. Pure.
 */
std::optional<std::int64_t> parse_http_date(std::string_view value) noexcept;

/**
 * The server's clock as seen from this device (Unity SDK `TombstackHttp` parity).
 *
 * Ingest requests are signed with a timestamp the server accepts only within
 * 300 s of its own clock, so a device whose clock is wrong by more would have
 * every signed request refused. Every ingest response carries the server's
 * `Date`: observe() learns the server-minus-device offset from it, and
 * now_unix_seconds() is the corrected time to sign with.
 *
 * A `Date` outside [2024, 2100) is ignored, so a captive portal or a broken
 * proxy answering cannot steer signing, and the offset only moves when it
 * disagrees by at least `hysteresis_seconds`, so the header's one-second
 * resolution and the request latency never make it jitter. Thread-safe.
 */
class ServerClock {
public:
    static constexpr std::int64_t hysteresis_seconds = 2;
    static constexpr std::int64_t min_plausible_unix = 1704067200;  // 2024-01-01T00:00:00Z
    static constexpr std::int64_t max_plausible_unix = 4102444800;  // 2100-01-01T00:00:00Z

    /** Learn from one response `Date` value. Returns true when the offset moved. */
    bool observe(std::string_view date_header, std::int64_t device_now_unix) noexcept;
    /** Same, against the device clock now. */
    bool observe(std::string_view date_header) noexcept;

    /** Server time minus device time, in seconds (0 until a `Date` is seen). */
    std::int64_t offset_seconds() const noexcept;
    /** Best estimate of the server's Unix time when the device clock reads `device_now_unix`. */
    std::int64_t now_unix_seconds(std::int64_t device_now_unix) const noexcept;
    /** Best estimate of the server's Unix time now: what an ingest request is signed with. */
    std::int64_t now_unix_seconds() const noexcept;

private:
    std::atomic<std::int64_t> offset_{0};
};

/** What a 401 from an ingest endpoint means for the payload that received it. */
enum class Unauthorized {
    /** Not a 401. */
    none,
    /** The signature or its timestamp was refused (`invalid_signature`), or the 401 carried no
     *  recognisable code (an intermediary answered). Almost always a wrong device clock, which the
     *  response's own `Date` corrects: retryable, never a reason to delete the payload. */
    signature,
    /** The SDK token itself was refused (`invalid_api_key`: revoked, rotated or unknown).
     *  Retrying with the same token cannot help this launch. */
    key,
};

/** Classify a signed ingest response (Unity `ClassifyUnauthorized` parity). Pure. */
Unauthorized classify_unauthorized(long status, std::string_view body) noexcept;

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_SERVER_CLOCK_H
