#ifndef TOMBSTONE_SRC_CLOCK_H
#define TOMBSTONE_SRC_CLOCK_H

#include <cstdint>
#include <string>

namespace tombstone {

/**
 * Format a Unix-epoch millisecond count as the wire timestamp:
 * "YYYY-MM-DDTHH:MM:SS.mmmZ" (ISO-8601, UTC, millisecond precision).
 * Pure function (no locale, no gmtime) so it is testable and thread-safe.
 */
std::string format_iso8601_utc_ms(std::int64_t ms_since_epoch);

/** Current wall-clock time in the wire format above. */
std::string now_iso8601_utc_ms();

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_CLOCK_H
