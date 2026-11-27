#include "delivery_policy.h"
#include "test_framework.h"

#include <cstdint>
#include <string>

using tombstone::parse_iso8601_utc_seconds;
using tombstone::settle_api_response;
using tombstone::settle_upload_response;
using tombstone::Settle;
using tombstone::spend_cap_pause_seconds;
using tombstone::Unauthorized;

namespace {

/** An ingest POST answered with `status` (a 401 carrying `unauthorized`). */
Settle ingest(long status, Unauthorized unauthorized = Unauthorized::none) {
    return settle_api_response(false, status, unauthorized, true);
}

/** A pull-request call (not an ingest POST) answered with `status`. */
Settle pull_request(long status, Unauthorized unauthorized = Unauthorized::none) {
    return settle_api_response(false, status, unauthorized, false);
}

bool same(Settle actual, Settle expected) { return actual == expected; }

constexpr std::int64_t kNow = 1'795'000'000;  // 2026-11-18T11:06:40Z

std::string cap_body(const std::string &resumes_at) {
    return R"({"success":false,"error":"spend_cap_reached","message":"Spending cap reached.",)"
           R"("resumesAt":")" + resumes_at + R"("})";
}

}  // namespace

TEST_CASE("delivery_policy", "2xx is delivered and no answer is retried") {
    CHECK(same(ingest(200), Settle::delivered));
    CHECK(same(ingest(201), Settle::delivered));
    CHECK(same(ingest(202), Settle::delivered));
    CHECK(same(settle_api_response(true, 0, Unauthorized::none, true), Settle::retry));
    CHECK(same(settle_api_response(true, 0, Unauthorized::none, false), Settle::retry));
}

TEST_CASE("delivery_policy", "408, 429 and 5xx are retried") {
    for (const long status : {408L, 429L, 500L, 502L, 503L, 504L}) {
        CHECK(same(ingest(status), Settle::retry));
        CHECK(same(pull_request(status), Settle::retry));
    }
}

TEST_CASE("delivery_policy", "a signature 401 is retried, a refused token stops the launch") {
    CHECK(same(ingest(401, Unauthorized::signature), Settle::retry));
    CHECK(same(ingest(401, Unauthorized::key), Settle::token_refused));
    CHECK(same(ingest(403), Settle::token_refused));
}

TEST_CASE("delivery_policy", "402 is the spending-cap stop on every API call") {
    CHECK(same(ingest(402), Settle::spend_cap));
    CHECK(same(pull_request(402), Settle::spend_cap));
}

TEST_CASE("delivery_policy", "a payload the server refuses is dropped, 404 included") {
    for (const long status : {400L, 404L, 405L, 409L, 410L, 413L, 422L}) {
        CHECK(same(ingest(status), Settle::drop));
    }
}

TEST_CASE("delivery_policy", "a refused pull-request call drops only that call") {
    // An ingest-only token may not create log pulls: that 403 must not stop ingestion.
    CHECK(same(pull_request(401, Unauthorized::key), Settle::drop));
    CHECK(same(pull_request(403), Settle::drop));
    CHECK(same(pull_request(404), Settle::drop));
}

TEST_CASE("delivery_policy", "an upload to the storage host is delivered, retried or dropped") {
    CHECK(same(settle_upload_response(false, 204), Settle::delivered));
    CHECK(same(settle_upload_response(true, 0), Settle::retry));
    CHECK(same(settle_upload_response(false, 503), Settle::retry));
    CHECK(same(settle_upload_response(false, 402), Settle::drop));  // not Tombstack's cap
    CHECK(same(settle_upload_response(false, 403), Settle::drop));
}

TEST_CASE("delivery_policy", "parses the server's UTC timestamps") {
    CHECK_EQ(parse_iso8601_utc_seconds("1970-01-01T00:00:00Z").value_or(-1), std::int64_t{0});
    CHECK_EQ(parse_iso8601_utc_seconds("2026-12-01T00:00:00.000Z").value_or(-1),
             std::int64_t{1'796'083'200});
    CHECK_EQ(parse_iso8601_utc_seconds("2028-02-29T23:59:59Z").value_or(-1),
             std::int64_t{1'835'481'599});
    CHECK_EQ(parse_iso8601_utc_seconds("2026-12-01T00:00:00.5Z").value_or(-1),
             std::int64_t{1'796'083'200});
}

TEST_CASE("delivery_policy", "rejects timestamps of any other shape") {
    for (const char *bad : {"", "2026-12-01", "2026-12-01T00:00:00", "2026-12-01 00:00:00Z",
                            "2026-12-01T00:00:00+01:00", "2026-13-01T00:00:00Z",
                            "2026-02-29T00:00:00Z", "2026-12-01T24:00:00Z", "2026-12-01T00:00:00.Z",
                            "2026-12-01T00:00:00.12345678901Z", "2026-1a-01T00:00:00Z"}) {
        CHECK(!parse_iso8601_utc_seconds(bad).has_value());
    }
}

TEST_CASE("delivery_policy", "the stop lasts until resumesAt, within a minute and an hour") {
    const std::string in_ten_minutes = "2026-11-18T11:16:40.000Z";
    CHECK_EQ(spend_cap_pause_seconds(cap_body(in_ten_minutes), kNow), std::int64_t{600});
    // The usual case: the period resumes days later, and the stop is re-checked hourly.
    CHECK_EQ(spend_cap_pause_seconds(cap_body("2026-12-01T00:00:00.000Z"), kNow),
             std::int64_t{3600});
    // Already past, or seconds away: at least a minute.
    CHECK_EQ(spend_cap_pause_seconds(cap_body("2026-11-18T11:06:50.000Z"), kNow),
             std::int64_t{60});
    CHECK_EQ(spend_cap_pause_seconds(cap_body("2026-11-01T00:00:00.000Z"), kNow),
             std::int64_t{60});
}

TEST_CASE("delivery_policy", "a 402 without a usable resumesAt stops for 15 minutes") {
    CHECK_EQ(spend_cap_pause_seconds("", kNow), std::int64_t{900});
    CHECK_EQ(spend_cap_pause_seconds("<html>Payment Required</html>", kNow), std::int64_t{900});
    CHECK_EQ(spend_cap_pause_seconds(R"({"error":"spend_cap_reached"})", kNow), std::int64_t{900});
    CHECK_EQ(spend_cap_pause_seconds(cap_body("soon"), kNow), std::int64_t{900});
    CHECK_EQ(spend_cap_pause_seconds(R"({"resumesAt":1796083200})", kNow), std::int64_t{900});
}

TEST_CASE("delivery_policy", "only the start of a large 402 body is read") {
    const std::string padding(tombstone::spend_cap_max_body_bytes, ' ');
    const std::string late = "{" + padding + R"("resumesAt":"2026-11-18T11:16:40.000Z"})";
    CHECK_EQ(spend_cap_pause_seconds(late, kNow), std::int64_t{900});
}
