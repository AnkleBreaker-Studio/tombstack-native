#include "clock.h"
#include "test_framework.h"

using tombstone::format_iso8601_utc_ms;
using tombstone::now_iso8601_utc_ms;

TEST_CASE("clock", "formats the unix epoch") {
    CHECK_EQ(format_iso8601_utc_ms(0), std::string{"1970-01-01T00:00:00.000Z"});
}

TEST_CASE("clock", "formats known timestamps with millisecond precision") {
    // 2023-11-14T22:13:20 UTC == 1700000000 seconds since the epoch.
    CHECK_EQ(format_iso8601_utc_ms(1700000000000LL), std::string{"2023-11-14T22:13:20.000Z"});
    CHECK_EQ(format_iso8601_utc_ms(1700000000123LL), std::string{"2023-11-14T22:13:20.123Z"});
    // Leap-year day: 2024-02-29.
    CHECK_EQ(format_iso8601_utc_ms(1709164800000LL), std::string{"2024-02-29T00:00:00.000Z"});
    // End-of-year boundary.
    CHECK_EQ(format_iso8601_utc_ms(1735689599999LL), std::string{"2024-12-31T23:59:59.999Z"});
}

TEST_CASE("clock", "now matches the wire shape") {
    const std::string now = now_iso8601_utc_ms();
    CHECK_EQ(now.size(), std::size_t{24});  // YYYY-MM-DDTHH:MM:SS.mmmZ
    CHECK_EQ(now[4], '-');
    CHECK_EQ(now[10], 'T');
    CHECK_EQ(now[19], '.');
    CHECK_EQ(now[23], 'Z');
}
