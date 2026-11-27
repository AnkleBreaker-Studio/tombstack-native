#include "server_clock.h"
#include "test_framework.h"

#include <cstdint>
#include <string>

using tombstone::classify_unauthorized;
using tombstone::parse_http_date;
using tombstone::ServerClock;
using tombstone::Unauthorized;

TEST_CASE("server_clock", "parses an IMF-fixdate Date header into Unix seconds") {
    // RFC 9110's own example, plus the value curl hands over (leading space, trailing CRLF).
    CHECK_EQ(parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT").value_or(-1),
             static_cast<std::int64_t>(784111777));
    CHECK_EQ(parse_http_date(" Thu, 26 Nov 2026 12:00:00 GMT\r\n").value_or(-1),
             static_cast<std::int64_t>(1795694400));
    CHECK_EQ(parse_http_date("Thu, 01 Jan 1970 00:00:00 GMT").value_or(-1),
             static_cast<std::int64_t>(0));
    CHECK_EQ(parse_http_date("Tue, 29 Feb 2028 23:59:59 GMT").value_or(-1),
             static_cast<std::int64_t>(1835481599));
}

TEST_CASE("server_clock", "rejects every other Date shape") {
    CHECK(!parse_http_date("").has_value());
    CHECK(!parse_http_date("Sunday, 06-Nov-94 08:49:37 GMT").has_value());  // RFC 850
    CHECK(!parse_http_date("Sun Nov  6 08:49:37 1994").has_value());        // asctime
    CHECK(!parse_http_date("Sun, 06 Nov 1994 08:49:37 UTC").has_value());
    CHECK(!parse_http_date("Sun, 06 Foo 1994 08:49:37 GMT").has_value());
    CHECK(!parse_http_date("Sun, 6 Nov 1994 08:49:37 GMT").has_value());
    CHECK(!parse_http_date("Sun, 06 Nov 1994 24:00:00 GMT").has_value());
    CHECK(!parse_http_date("Sun, 32 Nov 1994 08:49:37 GMT").has_value());
    CHECK(!parse_http_date("Xyz, 06 Nov 1994 08:49:37 GMT").has_value());
}

TEST_CASE("server_clock", "learns the server-minus-device offset and corrects the signing time") {
    ServerClock clock;
    const std::int64_t device_now = 1795694400;  // 2026-11-26T12:00:00Z on the device
    CHECK_EQ(clock.now_unix_seconds(device_now), device_now);  // no Date seen yet
    // The server is ten minutes ahead of this device.
    CHECK(clock.observe("Thu, 26 Nov 2026 12:10:00 GMT", device_now));
    CHECK_EQ(clock.offset_seconds(), static_cast<std::int64_t>(600));
    CHECK_EQ(clock.now_unix_seconds(device_now + 5), device_now + 605);
    // A device clock that runs ahead is corrected the other way.
    CHECK(clock.observe("Thu, 26 Nov 2026 11:00:00 GMT", device_now));
    CHECK_EQ(clock.offset_seconds(), static_cast<std::int64_t>(-3600));
}

TEST_CASE("server_clock", "ignores sub-hysteresis drift and implausible server dates") {
    ServerClock clock;
    const std::int64_t device_now = 1795694400;
    CHECK(!clock.observe("Thu, 26 Nov 2026 12:00:01 GMT", device_now));  // 1 s: latency, not skew
    CHECK_EQ(clock.offset_seconds(), static_cast<std::int64_t>(0));
    CHECK(clock.observe("Thu, 26 Nov 2026 12:00:02 GMT", device_now));   // 2 s: moves
    CHECK_EQ(clock.offset_seconds(), static_cast<std::int64_t>(2));
    // A captive portal or broken proxy must not steer signing.
    CHECK(!clock.observe("Thu, 01 Jan 1970 00:00:00 GMT", device_now));
    CHECK(!clock.observe("Fri, 01 Jan 2100 00:00:00 GMT", device_now));
    CHECK(!clock.observe("not a date", device_now));
    CHECK_EQ(clock.offset_seconds(), static_cast<std::int64_t>(2));
}

TEST_CASE("server_clock", "a 401 is a clock problem unless the body names a refused token") {
    CHECK(classify_unauthorized(202, "") == Unauthorized::none);
    CHECK(classify_unauthorized(400, R"({"code":"invalid_api_key"})") == Unauthorized::none);
    CHECK(classify_unauthorized(
              401, R"({"success":false,"error":"invalid signature","code":"invalid_signature"})") ==
          Unauthorized::signature);
    CHECK(classify_unauthorized(401, "") == Unauthorized::signature);  // an intermediary's bare 401
    CHECK(classify_unauthorized(
              401, R"({"success":false,"error":"invalid api key","code":"invalid_api_key"})") ==
          Unauthorized::key);
}
