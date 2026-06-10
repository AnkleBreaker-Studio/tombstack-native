#include "sdk_log.h"
#include "session_log.h"
#include "test_framework.h"

#include <fstream>

namespace fs = std::filesystem;
using tombstone::SdkLog;
using tombstone::SessionLog;

namespace {

std::string read_text(const fs::path &path) {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

}  // namespace

TEST_CASE("session_log", "appended lines flush in the documented format") {
    ttest::TempDir dir;
    SdkLog sdk_log;
    SessionLog log{sdk_log};
    log.configure(dir.path());
    log.rotate_for_new_session();

    log.append("2026-01-02T03:04:05.678Z", "Info", "hello world");
    log.append("2026-01-02T03:04:06.000Z", "Error", "boom");
    log.flush_now();

    const std::string content = read_text(dir.path() / "session.log");
    CHECK_EQ(content, std::string{"2026-01-02T03:04:05.678Z [Info] hello world\n"
                                  "2026-01-02T03:04:06.000Z [Error] boom\n"});
}

TEST_CASE("session_log", "rotation preserves the previous run and resets the current") {
    ttest::TempDir dir;
    SdkLog sdk_log;
    {
        SessionLog first{sdk_log};
        first.configure(dir.path());
        CHECK(!first.rotate_for_new_session());  // nothing to rotate yet
        first.append("t1", "Info", "from run one");
        first.flush_now();
    }
    SessionLog second{sdk_log};
    second.configure(dir.path());
    CHECK(second.rotate_for_new_session());  // previous log now exists
    CHECK(fs::exists(dir.path() / "previous-session.log"));
    CHECK(!fs::exists(dir.path() / "session.log"));
    const std::string previous = read_text(dir.path() / "previous-session.log");
    CHECK(previous.find("from run one") != std::string::npos);

    const auto bytes = second.read_previous();
    CHECK_EQ(std::string(bytes.begin(), bytes.end()), previous);
}

TEST_CASE("session_log", "stale previous logs are replaced on rotation") {
    ttest::TempDir dir;
    SdkLog sdk_log;
    {
        std::ofstream stale{dir.path() / "previous-session.log", std::ios::binary};
        stale << "stale run";
    }
    {
        std::ofstream current{dir.path() / "session.log", std::ios::binary};
        current << "fresh run";
    }
    SessionLog log{sdk_log};
    log.configure(dir.path());
    CHECK(log.rotate_for_new_session());
    const std::string previous = read_text(dir.path() / "previous-session.log");
    CHECK_EQ(previous, std::string{"fresh run"});
}

TEST_CASE("session_log", "trims from the front past the cap keeping newest lines") {
    ttest::TempDir dir;
    SdkLog sdk_log;
    SessionLog log{sdk_log};
    log.configure(dir.path());
    log.rotate_for_new_session();

    // Write well past the 512 KB cap in flushed batches (the pending buffer
    // itself is capped at 64 KB).
    const std::string filler(1000, 'x');
    for (int line = 0; line < 700; ++line) {
        log.append("2026-01-02T03:04:05.678Z", "Info",
                   "line-" + std::to_string(line) + " " + filler);
        if (line % 30 == 0) {
            log.flush_now();
        }
    }
    log.flush_now();

    const std::string content = read_text(dir.path() / "session.log");
    CHECK(content.size() <= SessionLog::max_log_bytes);
    CHECK(!content.empty());
    // Newest line survived; the very first line was trimmed away.
    CHECK(content.find("line-699 ") != std::string::npos);
    CHECK(content.find("line-0 ") == std::string::npos);
    // Trim is line-aligned: the file starts with a timestamp, not a torn line.
    CHECK_EQ(content.substr(0, 4), std::string{"2026"});
}

TEST_CASE("session_log", "read_current includes unflushed lines") {
    ttest::TempDir dir;
    SdkLog sdk_log;
    SessionLog log{sdk_log};
    log.configure(dir.path());
    log.rotate_for_new_session();

    log.append("t", "Info", "pending line");
    const auto bytes = log.read_current();  // flushes first
    const std::string content(bytes.begin(), bytes.end());
    CHECK(content.find("pending line") != std::string::npos);
}
