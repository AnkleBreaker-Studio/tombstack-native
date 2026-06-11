#include "payloads.h"
#include "test_framework.h"

#include <string>
#include <vector>

using tombstone::Breadcrumb;
using tombstone::BugReportPayload;
using tombstone::build_batch_envelope;
using tombstone::build_bug_report_json;
using tombstone::build_crash_json;
using tombstone::build_event_json;
using tombstone::build_heartbeat_json;
using tombstone::build_metric_json;
using tombstone::CrashPayload;
using tombstone::EventPayload;
using tombstone::HeartbeatPayload;
using tombstone::MetricPayload;

TEST_CASE("payloads", "crash with every field is byte exact") {
    CrashPayload payload;
    payload.occurred_at_iso = "2026-01-02T03:04:05.678Z";
    payload.build_version = "1.2.3";
    payload.os = "windows";
    payload.arch = "x64";
    payload.signature = "deadbeef";
    payload.stack_hint = "NullReference in Boot";
    payload.stack_trace = "frame1\nframe2";
    payload.breadcrumbs = {Breadcrumb{"2026-01-02T03:04:00.000Z", "Info", "loaded level 3"}};
    payload.user_id = "user-1";
    payload.steam_id = "7656119";
    payload.log = true;
    CHECK_EQ(build_crash_json(payload),
             std::string{R"({"occurredAtIso":"2026-01-02T03:04:05.678Z",)"
                         R"("buildVersion":"1.2.3","os":"windows","arch":"x64",)"
                         R"("signature":"deadbeef","stackHint":"NullReference in Boot",)"
                         R"("stackTrace":"frame1\nframe2",)"
                         R"("breadcrumbs":[{"tsIso":"2026-01-02T03:04:00.000Z",)"
                         R"("level":"Info","message":"loaded level 3"}],)"
                         R"("userId":"user-1","steamId":"7656119","log":true})"});
}

TEST_CASE("payloads", "minimal crash omits optionals but always carries log") {
    CrashPayload payload;
    payload.occurred_at_iso = "2026-01-02T03:04:05.678Z";
    payload.build_version = "1.2.3";
    payload.os = "linux";
    payload.arch = "arm64";
    payload.signature = "unclean-shutdown";
    payload.stack_hint = "hint";
    payload.log = false;  // "no log" is explicit, never an absent field
    CHECK_EQ(build_crash_json(payload),
             std::string{R"({"occurredAtIso":"2026-01-02T03:04:05.678Z",)"
                         R"("buildVersion":"1.2.3","os":"linux","arch":"arm64",)"
                         R"("signature":"unclean-shutdown","stackHint":"hint","log":false})"});
}

TEST_CASE("payloads", "crash clamps oversized strings to the schema maxima") {
    CrashPayload payload;
    payload.occurred_at_iso = "2026-01-02T03:04:05.678Z";
    payload.build_version = "1.0.0";
    payload.os = "windows";
    payload.arch = "x64";
    payload.signature = std::string(200, 's');
    payload.stack_hint = std::string(600, 'h');
    payload.stack_trace = std::string(9000, 't');
    const std::string json = build_crash_json(payload);
    CHECK(json.find('"' + std::string(128, 's') + '"') != std::string::npos);
    CHECK(json.find(std::string(129, 's')) == std::string::npos);
    CHECK(json.find('"' + std::string(512, 'h') + '"') != std::string::npos);
    CHECK(json.find(std::string(513, 'h')) == std::string::npos);
    CHECK(json.find(std::string(8193, 't')) == std::string::npos);
}

TEST_CASE("payloads", "bug report is byte exact and omits empty category") {
    BugReportPayload payload;
    payload.occurred_at_iso = "2026-01-02T03:04:05.678Z";
    payload.build_version = "2.0.0";
    payload.os = "macos";
    payload.arch = "arm64";
    payload.message = "button does nothing";
    payload.log = true;
    CHECK_EQ(build_bug_report_json(payload),
             std::string{R"({"occurredAtIso":"2026-01-02T03:04:05.678Z",)"
                         R"("buildVersion":"2.0.0","os":"macos","arch":"arm64",)"
                         R"("message":"button does nothing","log":true})"});

    payload.category = "ui";
    payload.user_id = "u9";
    CHECK_EQ(build_bug_report_json(payload),
             std::string{R"({"occurredAtIso":"2026-01-02T03:04:05.678Z",)"
                         R"("buildVersion":"2.0.0","os":"macos","arch":"arm64",)"
                         R"("category":"ui","message":"button does nothing",)"
                         R"("userId":"u9","log":true})"});
}

TEST_CASE("payloads", "event serializes attributes in insertion order") {
    EventPayload payload;
    payload.occurred_at_iso = "2026-01-02T03:04:05.678Z";
    payload.build_version = "1.0.0";
    payload.os = "windows";
    payload.arch = "x64";
    payload.name = "level_complete";
    payload.user_id = "u1";
    payload.attributes = {{"level", "3"}, {"boss", "true"}};
    CHECK_EQ(build_event_json(payload),
             std::string{R"({"occurredAtIso":"2026-01-02T03:04:05.678Z",)"
                         R"("buildVersion":"1.0.0","os":"windows","arch":"x64",)"
                         R"("name":"level_complete","userId":"u1",)"
                         R"("attributes":{"level":"3","boss":"true"}})"});
}

TEST_CASE("payloads", "event omits userId and attributes when absent") {
    EventPayload payload;
    payload.occurred_at_iso = "2026-01-02T03:04:05.678Z";
    payload.build_version = "1.0.0";
    payload.os = "other";
    payload.arch = "other";
    payload.name = "boot";
    const std::string json = build_event_json(payload);
    CHECK_EQ(json, std::string{R"({"occurredAtIso":"2026-01-02T03:04:05.678Z",)"
                               R"("buildVersion":"1.0.0","os":"other","arch":"other",)"
                               R"("name":"boot"})"});
    // Absent optionals are OMITTED — never serialized as "".
    CHECK(json.find("\"level\"") == std::string::npos);
    CHECK(json.find("\"\"") == std::string::npos);
}

TEST_CASE("payloads", "event caps attributes at 32 entries") {
    EventPayload payload;
    payload.occurred_at_iso = "2026-01-02T03:04:05.678Z";
    payload.build_version = "1.0.0";
    payload.os = "windows";
    payload.arch = "x64";
    payload.name = "spam";
    for (int i = 0; i < 40; ++i) {
        payload.attributes.emplace_back("k" + std::to_string(i), "v");
    }
    const std::string json = build_event_json(payload);
    CHECK(json.find("\"k31\"") != std::string::npos);
    CHECK(json.find("\"k32\"") == std::string::npos);
}

TEST_CASE("payloads", "heartbeat is byte exact with and without userId") {
    HeartbeatPayload payload;
    payload.session_id = "a1b2c3d4";
    payload.occurred_at_iso = "2026-01-02T03:04:05.678Z";
    payload.build_version = "1.0.0";
    payload.os = "windows";
    payload.arch = "x86";
    CHECK_EQ(build_heartbeat_json(payload),
             std::string{R"({"sessionId":"a1b2c3d4",)"
                         R"("occurredAtIso":"2026-01-02T03:04:05.678Z",)"
                         R"("buildVersion":"1.0.0","os":"windows","arch":"x86"})"});

    payload.user_id = "player-7";
    CHECK_EQ(build_heartbeat_json(payload),
             std::string{R"({"sessionId":"a1b2c3d4",)"
                         R"("occurredAtIso":"2026-01-02T03:04:05.678Z",)"
                         R"("buildVersion":"1.0.0","os":"windows","arch":"x86",)"
                         R"("userId":"player-7"})"});
}

TEST_CASE("payloads", "crash stamps correlation dimensions and omits empty ones") {
    CrashPayload payload;
    payload.occurred_at_iso = "2026-01-02T03:04:05.678Z";
    payload.build_version = "1.0.0";
    payload.os = "linux";
    payload.arch = "x64";
    payload.signature = "deadbeef";
    payload.stack_hint = "hint";
    payload.log = false;
    payload.role = "server";
    payload.server_id = "srv-eu-1";
    payload.match_id = "m-42";
    payload.session_id = "sess-9";
    CHECK_EQ(build_crash_json(payload),
             std::string{R"({"occurredAtIso":"2026-01-02T03:04:05.678Z",)"
                         R"("buildVersion":"1.0.0","os":"linux","arch":"x64",)"
                         R"("signature":"deadbeef","stackHint":"hint","log":false,)"
                         R"("role":"server","serverId":"srv-eu-1","matchId":"m-42",)"
                         R"("sessionId":"sess-9"})"});

    // A client crash with no server/match context omits role/serverId/matchId
    // (never an empty-string enum) but still carries the session id.
    CrashPayload client_crash;
    client_crash.occurred_at_iso = "2026-01-02T03:04:05.678Z";
    client_crash.build_version = "1.0.0";
    client_crash.os = "windows";
    client_crash.arch = "x64";
    client_crash.signature = "deadbeef";
    client_crash.stack_hint = "hint";
    client_crash.log = false;
    client_crash.session_id = "sess-9";
    const std::string client_json = build_crash_json(client_crash);
    CHECK_EQ(client_json,
             std::string{R"({"occurredAtIso":"2026-01-02T03:04:05.678Z",)"
                         R"("buildVersion":"1.0.0","os":"windows","arch":"x64",)"
                         R"("signature":"deadbeef","stackHint":"hint","log":false,)"
                         R"("sessionId":"sess-9"})"});
    CHECK(client_json.find("\"role\"") == std::string::npos);
    CHECK(client_json.find("\"serverId\"") == std::string::npos);
    CHECK(client_json.find("\"matchId\"") == std::string::npos);
}

TEST_CASE("payloads", "bug report stamps correlation dimensions") {
    BugReportPayload payload;
    payload.occurred_at_iso = "2026-01-02T03:04:05.678Z";
    payload.build_version = "2.0.0";
    payload.os = "macos";
    payload.arch = "arm64";
    payload.message = "button does nothing";
    payload.log = true;
    payload.role = "server";
    payload.server_id = "srv-eu-1";
    payload.match_id = "m-42";
    payload.session_id = "sess-9";
    CHECK_EQ(build_bug_report_json(payload),
             std::string{R"({"occurredAtIso":"2026-01-02T03:04:05.678Z",)"
                         R"("buildVersion":"2.0.0","os":"macos","arch":"arm64",)"
                         R"("message":"button does nothing","log":true,)"
                         R"("role":"server","serverId":"srv-eu-1","matchId":"m-42",)"
                         R"("sessionId":"sess-9"})"});
}

TEST_CASE("payloads", "metric is byte exact with value, unit, and correlation") {
    MetricPayload payload;
    payload.name = "tickrate";
    payload.value = 60.0;  // integral double -> "60", no trailing ".0"
    payload.unit = "hz";
    payload.occurred_at_iso = "2026-01-02T03:04:05.678Z";
    payload.build_version = "1.0.0";
    payload.os = "windows";
    payload.arch = "x64";
    payload.role = "server";
    payload.server_id = "srv-1";
    payload.match_id = "m-1";
    // sessionId left empty -> must be OMITTED, never sent as "".
    const std::string json = build_metric_json(payload);
    CHECK_EQ(json, std::string{R"({"name":"tickrate","value":60,"unit":"hz",)"
                               R"("occurredAtIso":"2026-01-02T03:04:05.678Z",)"
                               R"("buildVersion":"1.0.0","os":"windows","arch":"x64",)"
                               R"("role":"server","serverId":"srv-1","matchId":"m-1"})"});
    CHECK(json.find("\"sessionId\"") == std::string::npos);
}

TEST_CASE("payloads", "minimal metric carries a fractional value and omits optionals") {
    MetricPayload payload;
    payload.name = "rtt";
    payload.value = 42.5;
    payload.occurred_at_iso = "2026-01-02T03:04:05.678Z";
    payload.build_version = "1.0.0";
    payload.os = "linux";
    payload.arch = "arm64";
    const std::string json = build_metric_json(payload);
    CHECK_EQ(json, std::string{R"({"name":"rtt","value":42.5,)"
                               R"("occurredAtIso":"2026-01-02T03:04:05.678Z",)"
                               R"("buildVersion":"1.0.0","os":"linux","arch":"arm64"})"});
    // No unit / userId / correlation -> all OMITTED.
    CHECK(json.find("\"unit\"") == std::string::npos);
    CHECK(json.find("\"userId\"") == std::string::npos);
    CHECK(json.find("\"role\"") == std::string::npos);
    CHECK(json.find("\"\"") == std::string::npos);
}

TEST_CASE("payloads", "batch envelope wraps pre-serialized items verbatim") {
    const std::vector<std::string> items = {R"({"name":"a"})", R"({"name":"b"})"};
    CHECK_EQ(build_batch_envelope("2026-06-11T00:00:00.000Z", items),
             std::string{R"({"sentAtIso":"2026-06-11T00:00:00.000Z",)"
                         R"("items":[{"name":"a"},{"name":"b"}]})"});
}

TEST_CASE("payloads", "batch envelope with no items is an empty array") {
    CHECK_EQ(build_batch_envelope("2026-06-11T00:00:00.000Z", {}),
             std::string{R"({"sentAtIso":"2026-06-11T00:00:00.000Z","items":[]})"});
}

TEST_CASE("payloads", "event stamps correlation dimensions after attributes") {
    EventPayload payload;
    payload.occurred_at_iso = "2026-01-02T03:04:05.678Z";
    payload.build_version = "1.0.0";
    payload.os = "windows";
    payload.arch = "x64";
    payload.name = "spawn";
    payload.match_id = "m-42";
    payload.session_id = "sess-9";
    // role + serverId empty -> omitted; matchId/sessionId present.
    const std::string json = build_event_json(payload);
    CHECK_EQ(json, std::string{R"({"occurredAtIso":"2026-01-02T03:04:05.678Z",)"
                               R"("buildVersion":"1.0.0","os":"windows","arch":"x64",)"
                               R"("name":"spawn","matchId":"m-42","sessionId":"sess-9"})"});
    CHECK(json.find("\"role\"") == std::string::npos);
    CHECK(json.find("\"serverId\"") == std::string::npos);
}
