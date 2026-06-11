#include "json_scan.h"
#include "test_framework.h"

using tombstone::find_bool_field;
using tombstone::find_log_upload_url;
using tombstone::find_pending_requests;
using tombstone::find_string_field;

namespace {

// Shape returned by POST /api/v1/ingest/crashes when "log": true was sent.
constexpr const char *ingest_response =
    R"({"success":true,"data":{"crashId":"01HZX4","logUpload":)"
    R"({"url":"https://s3.example/presigned?sig=a%2Fb","key":"logs/g/c.log",)"
    R"("method":"PUT","headers":{"Content-Type":"text/plain"}}}})";

}  // namespace

TEST_CASE("json_scan", "finds string fields with unescaping") {
    CHECK_EQ(find_string_field(R"({"a":"hello"})", "a").value_or("?"), std::string{"hello"});
    CHECK_EQ(find_string_field(R"({"a" : "spaced"})", "a").value_or("?"),
             std::string{"spaced"});
    CHECK_EQ(find_string_field(R"({"a":"q\"b\\c\nd"})", "a").value_or("?"),
             std::string{"q\"b\\c\nd"});
    // é decodes to the 2-byte UTF-8 sequence 0xC3 0xA9.
    CHECK_EQ(find_string_field("{\"a\":\"A\\u00e9\"}", "a").value_or("?"),
             std::string{"A\xC3\xA9"});
    CHECK(!find_string_field(R"({"a":42})", "a").has_value());
    CHECK(!find_string_field(R"({"a":"x"})", "missing").has_value());
}

TEST_CASE("json_scan", "finds boolean fields") {
    CHECK_EQ(find_bool_field(R"({"log":true})", "log").value_or(false), true);
    CHECK_EQ(find_bool_field(R"({"log": false})", "log").value_or(true), false);
    CHECK(!find_bool_field(R"({"log":"true"})", "log").has_value());
    CHECK(!find_bool_field(R"({"other":true})", "log").has_value());
}

TEST_CASE("json_scan", "extracts the presigned log upload url") {
    const auto url = find_log_upload_url(ingest_response);
    CHECK(url.has_value());
    CHECK_EQ(*url, std::string{"https://s3.example/presigned?sig=a%2Fb"});
}

TEST_CASE("json_scan", "returns nullopt when no log slot was granted") {
    CHECK(!find_log_upload_url(R"({"success":true,"data":{"crashId":"01HZX4"}})").has_value());
    // A stray top-level "url" must not be mistaken for logUpload.url.
    CHECK(!find_log_upload_url(R"({"url":"https://wrong.example","data":{}})").has_value());
}

TEST_CASE("json_scan", "stays inside the logUpload object") {
    // "url" appears AFTER logUpload closes -> must not be picked up.
    const char *json = R"({"data":{"logUpload":{"key":"k"},"url":"https://outside"}})";
    CHECK(!find_log_upload_url(json).has_value());
}

TEST_CASE("json_scan", "parses the heartbeat-ack pending requests") {
    const char *ack =
        R"({"success":true,"data":{"accepted":true,"pendingRequests":[)"
        R"({"requestId":"r1","targetType":"userId","targetValue":"player-7"},)"
        R"({"requestId":"r2","targetType":"server","targetValue":"srv-eu-1"}]}})";
    const auto pending = find_pending_requests(ack);
    CHECK_EQ(pending.size(), static_cast<std::size_t>(2));
    CHECK_EQ(pending[0].request_id, std::string{"r1"});
    CHECK_EQ(pending[0].target_type, std::string{"userId"});
    CHECK_EQ(pending[0].target_value, std::string{"player-7"});
    CHECK_EQ(pending[1].request_id, std::string{"r2"});
    CHECK_EQ(pending[1].target_type, std::string{"server"});
    CHECK_EQ(pending[1].target_value, std::string{"srv-eu-1"});
}

TEST_CASE("json_scan", "returns no pending requests for an empty or absent array") {
    CHECK_EQ(
        find_pending_requests(R"({"success":true,"data":{"accepted":true,"pendingRequests":[]}})")
            .size(),
        static_cast<std::size_t>(0));
    CHECK_EQ(find_pending_requests(R"({"success":true,"data":{"accepted":true}})").size(),
             static_cast<std::size_t>(0));
}
