#include "sdk_log.h"
#include "sidecar_queue.h"
#include "test_framework.h"

#include <fstream>

namespace fs = std::filesystem;
using tombstone::SdkLog;
using tombstone::SidecarKind;
using tombstone::SidecarQueue;

namespace {
constexpr const char *crash_body =
    R"({"occurredAtIso":"2026-01-02T03:04:05.678Z","buildVersion":"1.0.0",)"
    R"("os":"windows","arch":"x64","signature":"s","stackHint":"h","log":false})";
}  // namespace

TEST_CASE("sidecar", "writes crash payloads into pending/crashes as .json") {
    ttest::TempDir dir;
    SdkLog log;
    SidecarQueue queue{log};
    queue.configure(dir.path());

    const fs::path file = queue.write(SidecarKind::crash, crash_body);
    CHECK(!file.empty());
    CHECK(fs::exists(file));
    CHECK_EQ(file.extension().string(), std::string{".json"});
    CHECK_EQ(file.parent_path().filename().string(), std::string{"crashes"});
    CHECK_EQ(file.parent_path().parent_path().filename().string(), std::string{"pending"});
}

TEST_CASE("sidecar", "file content is the raw ingest body (uploader compatible)") {
    ttest::TempDir dir;
    SdkLog log;
    SidecarQueue queue{log};
    queue.configure(dir.path());

    const fs::path file = queue.write(SidecarKind::crash, crash_body);
    std::ifstream in{file, std::ios::binary};
    const std::string content{std::istreambuf_iterator<char>{in},
                              std::istreambuf_iterator<char>{}};
    // No wrapper/envelope: tools/uploader/upload.mjs POSTs this byte-for-byte.
    CHECK_EQ(content, std::string{crash_body});
}

TEST_CASE("sidecar", "kinds map to their endpoint directories") {
    CHECK_EQ(std::string{SidecarQueue::kind_dir_name(SidecarKind::crash)},
             std::string{"crashes"});
    CHECK_EQ(std::string{SidecarQueue::kind_dir_name(SidecarKind::bug_report)},
             std::string{"bug-reports"});
    CHECK_EQ(std::string{SidecarQueue::kind_dir_name(SidecarKind::event)},
             std::string{"events"});
}

TEST_CASE("sidecar", "scan returns every pending record with its body and kind") {
    ttest::TempDir dir;
    SdkLog log;
    SidecarQueue queue{log};
    queue.configure(dir.path());

    queue.write(SidecarKind::crash, crash_body);
    queue.write(SidecarKind::bug_report, R"({"message":"m"})");
    queue.write(SidecarKind::event, R"({"name":"e"})");

    const auto records = queue.scan();
    CHECK_EQ(records.size(), std::size_t{3});
    int crashes = 0;
    int bugs = 0;
    int events = 0;
    for (const auto &record : records) {
        CHECK(!record.body.empty());
        CHECK(fs::exists(record.path));
        if (record.kind == SidecarKind::crash) {
            ++crashes;
            CHECK_EQ(record.body, std::string{crash_body});
        } else if (record.kind == SidecarKind::bug_report) {
            ++bugs;
        } else {
            ++events;
        }
    }
    CHECK_EQ(crashes, 1);
    CHECK_EQ(bugs, 1);
    CHECK_EQ(events, 1);
}

TEST_CASE("sidecar", "remove deletes the backing file") {
    ttest::TempDir dir;
    SdkLog log;
    SidecarQueue queue{log};
    queue.configure(dir.path());

    const fs::path file = queue.write(SidecarKind::event, R"({"name":"e"})");
    CHECK(fs::exists(file));
    queue.remove(file);
    CHECK(!fs::exists(file));
    CHECK(queue.scan().empty());
}

TEST_CASE("sidecar", "per kind capacity is enforced") {
    ttest::TempDir dir;
    SdkLog log;
    SidecarQueue queue{log};
    queue.configure(dir.path());

    for (std::size_t i = 0; i < SidecarQueue::max_files_per_kind; ++i) {
        CHECK(!queue.write(SidecarKind::event, R"({"name":"e"})").empty());
    }
    // The 65th write is refused (bounded queue) — but other kinds still work.
    CHECK(queue.write(SidecarKind::event, R"({"name":"overflow"})").empty());
    CHECK(!queue.write(SidecarKind::crash, crash_body).empty());
}
