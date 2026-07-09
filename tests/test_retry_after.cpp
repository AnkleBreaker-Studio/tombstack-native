#include "retry_after.h"
#include "test_framework.h"

#include <chrono>

using tombstone::max_retry_after_seconds;
using tombstone::parse_retry_after_seconds;
using tombstone::retry_delay;

using std::chrono::seconds;

TEST_CASE("retry_after", "parses the plain seconds form") {
    CHECK_EQ(parse_retry_after_seconds("120"), 120L);
    CHECK_EQ(parse_retry_after_seconds("0"), 0L);
    CHECK_EQ(parse_retry_after_seconds("7"), 7L);
}

TEST_CASE("retry_after", "tolerates surrounding whitespace and CRLF") {
    // curl hands the header callback the raw line remainder, e.g. " 30\r\n".
    CHECK_EQ(parse_retry_after_seconds(" 30\r\n"), 30L);
    CHECK_EQ(parse_retry_after_seconds("\t45 "), 45L);
}

TEST_CASE("retry_after", "rejects absent, malformed, and http-date values") {
    CHECK_EQ(parse_retry_after_seconds(""), -1L);
    CHECK_EQ(parse_retry_after_seconds("   "), -1L);
    CHECK_EQ(parse_retry_after_seconds("abc"), -1L);
    CHECK_EQ(parse_retry_after_seconds("12x"), -1L);
    CHECK_EQ(parse_retry_after_seconds("-5"), -1L);
    CHECK_EQ(parse_retry_after_seconds("1.5"), -1L);
    // The HTTP-date form is valid HTTP but not the seconds form: fall back.
    CHECK_EQ(parse_retry_after_seconds("Wed, 21 Oct 2026 07:28:00 GMT"), -1L);
    // Absurdly long digit runs are rejected rather than overflowed.
    CHECK_EQ(parse_retry_after_seconds("9999999999999999"), -1L);
}

TEST_CASE("retry_after", "delay without a header is the plain backoff ladder") {
    CHECK_EQ(retry_delay(1, -1).count(), seconds{2}.count());
    CHECK_EQ(retry_delay(2, -1).count(), seconds{4}.count());
    CHECK_EQ(retry_delay(3, -1).count(), seconds{8}.count());
    CHECK_EQ(retry_delay(4, -1).count(), seconds{16}.count());
}

TEST_CASE("retry_after", "delay is the max of backoff and retry-after") {
    // A Retry-After above the backoff wins...
    CHECK_EQ(retry_delay(1, 60).count(), seconds{60}.count());
    // ...but a Retry-After below the backoff never SHORTENS the wait.
    CHECK_EQ(retry_delay(3, 1).count(), seconds{8}.count());
    // Equal values are a no-op either way.
    CHECK_EQ(retry_delay(2, 4).count(), seconds{4}.count());
    // A zero Retry-After ("retry now") still respects the backoff floor.
    CHECK_EQ(retry_delay(4, 0).count(), seconds{16}.count());
}

TEST_CASE("retry_after", "delay is capped at 300 seconds") {
    CHECK_EQ(retry_delay(1, 1000).count(), seconds{max_retry_after_seconds}.count());
    CHECK_EQ(retry_delay(4, 999999999).count(), seconds{max_retry_after_seconds}.count());
    CHECK_EQ(retry_delay(1, max_retry_after_seconds).count(),
             seconds{max_retry_after_seconds}.count());
}

TEST_CASE("retry_after", "out-of-range attempts clamp defensively") {
    CHECK_EQ(retry_delay(0, -1).count(), seconds{2}.count());
    CHECK_EQ(retry_delay(-3, -1).count(), seconds{2}.count());
    CHECK_EQ(retry_delay(9, -1).count(), seconds{32}.count());  // clamped to attempt 5
}
