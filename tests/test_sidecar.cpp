#include "sdk_log.h"
#include "sidecar_queue.h"
#include "test_framework.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>

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
    CHECK_EQ(std::string{SidecarQueue::kind_dir_name(SidecarKind::event_batch)},
             std::string{"events-batch"});
    CHECK_EQ(std::string{SidecarQueue::kind_dir_name(SidecarKind::metric_batch)},
             std::string{"metrics-batch"});
}

TEST_CASE("sidecar", "batch envelopes are kept, scanned and cleared like reports") {
    ttest::TempDir dir;
    SdkLog log;
    SidecarQueue queue{log};
    queue.configure(dir.path());
    queue.write(SidecarKind::event_batch, R"({"sentAtIso":"t","items":[{"name":"e"}]})");
    queue.write(SidecarKind::metric_batch, R"({"sentAtIso":"t","items":[{"name":"m"}]})");
    CHECK(fs::is_directory(dir.path() / "pending" / "events-batch"));
    CHECK(fs::is_directory(dir.path() / "pending" / "metrics-batch"));
    const auto records = queue.scan();
    CHECK_EQ(records.size(), std::size_t{2});
    CHECK_EQ(queue.pending_count(), std::size_t{2});
    queue.clear();
    CHECK_EQ(queue.pending_count(), std::size_t{0});
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

TEST_CASE("sidecar", "clear deletes every pending payload of every kind") {
    ttest::TempDir dir;
    SdkLog log;
    SidecarQueue queue{log};
    queue.configure(dir.path());

    queue.write(SidecarKind::crash, crash_body);
    queue.write(SidecarKind::bug_report, R"({"message":"m"})");
    queue.write(SidecarKind::event, R"({"name":"e"})");
    CHECK_EQ(queue.pending_count(), std::size_t{3});
    queue.clear();
    CHECK_EQ(queue.pending_count(), std::size_t{0});
    CHECK(queue.scan().empty());
    CHECK(!queue.write(SidecarKind::crash, crash_body).empty());  // still usable afterwards
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

namespace {

std::int64_t now_ms() {
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count());
}

constexpr std::int64_t day_ms = 86'400'000;

void write_text(const fs::path &path, const char *text) {
    std::ofstream out{path, std::ios::binary};
    out << text;
}

std::size_t files_with_extension(const fs::path &dir, const char *extension) {
    std::size_t count = 0;
    for (const auto &entry : fs::directory_iterator{dir}) {
        count += entry.path().extension() == extension ? 1 : 0;
    }
    return count;
}

}  // namespace

TEST_CASE("sidecar", "a payload is complete before it carries its .json name") {
    ttest::TempDir dir;
    SdkLog log;
    SidecarQueue queue{log};
    queue.configure(dir.path());

    CHECK(!queue.write(SidecarKind::crash, crash_body).empty());
    const fs::path crashes = dir.path() / "pending" / "crashes";
    CHECK_EQ(files_with_extension(crashes, ".json"), std::size_t{1});
    CHECK_EQ(files_with_extension(crashes, ".tmp"), std::size_t{0});
}

TEST_CASE("sidecar", "an interrupted write is never sent, and is cleaned up once stale") {
    ttest::TempDir dir;
    SdkLog log;
    SidecarQueue queue{log};
    queue.configure(dir.path());
    const fs::path crashes = dir.path() / "pending" / "crashes";
    const fs::path fresh = crashes / (std::to_string(now_ms()) + "-0000aaaa.json.tmp");
    const fs::path stale = crashes / (std::to_string(now_ms()) + "-0000bbbb.json.tmp");
    for (const fs::path &path : {fresh, stale}) {
        write_text(path, R"({"signature":"trunc)");
    }
    fs::last_write_time(stale, fs::file_time_type::clock::now() - std::chrono::hours{2});

    CHECK(queue.scan().empty());
    CHECK(fs::exists(fresh));   // another process of the game may still be writing it
    CHECK(!fs::exists(stale));  // left by a write that never finished
}

TEST_CASE("sidecar", "a payload older than 90 days is deleted unsent") {
    ttest::TempDir dir;
    SdkLog log;
    SidecarQueue queue{log};
    queue.configure(dir.path());
    const fs::path crashes = dir.path() / "pending" / "crashes";
    const fs::path old_file = crashes / (std::to_string(now_ms() - 91 * day_ms) + "-0000cccc.json");
    const fs::path kept_file = crashes / (std::to_string(now_ms() - 89 * day_ms) + "-0000dddd.json");
    for (const fs::path &path : {old_file, kept_file}) {
        write_text(path, crash_body);
    }

    const auto records = queue.scan();
    CHECK_EQ(records.size(), std::size_t{1});
    CHECK(records.size() == 1 && records[0].path == kept_file);
    CHECK(!fs::exists(old_file));
}

TEST_CASE("sidecar", "only a stamped name can expire") {
    const std::int64_t now = 1'795'000'000'000;
    CHECK(SidecarQueue::expired("1-00000000.json", now));
    CHECK(SidecarQueue::expired(std::to_string(now - 91 * day_ms) + "-1a2b3c4d.json", now));
    CHECK(!SidecarQueue::expired(std::to_string(now - 89 * day_ms) + "-1a2b3c4d.json", now));
    CHECK(!SidecarQueue::expired(std::to_string(now + day_ms) + "-1a2b3c4d.json", now));
    CHECK(!SidecarQueue::expired("report.json", now));
    CHECK(!SidecarQueue::expired("12345.json", now));
    CHECK(!SidecarQueue::expired("", now));
    CHECK(!SidecarQueue::expired("1234567890123456789-00.json", now));
}
