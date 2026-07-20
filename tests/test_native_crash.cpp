#include "native_crash.h"
#include "payloads.h"
#include "sdk_log.h"
#include "test_framework.h"

#include <fstream>
#include <string>

namespace fs = std::filesystem;
using tombstone::NativeCrashDump;
using tombstone::SdkLog;
namespace nc = tombstone::native_crash;

// --------------------------------------------------------------------------
// Pure parser (cross-platform): the dump text is fixed-format and must survive
// a truncated tail (a crash mid-write) yet reject non-dump files.
// --------------------------------------------------------------------------

TEST_CASE("native_crash", "parse_dump round-trips a full dump") {
    const std::string text =
        "tombstone-native-crash 1\n"
        "signal 11\n"
        "code 1\n"
        "fault 0x0\n"
        "frame libgame.so 0x1234 aabbccdd\n"
        "frame main 0x40 \n"
        "end\n";
    const auto dump = nc::parse_dump(text);
    CHECK(dump.has_value());
    CHECK_EQ(dump->signal_no, 11);
    CHECK_EQ(dump->code, 1);
    CHECK_EQ(dump->fault_addr, static_cast<std::uint64_t>(0));
    CHECK_EQ(dump->frames.size(), static_cast<std::size_t>(2));
    CHECK_EQ(dump->frames[0].module, std::string{"libgame.so"});
    CHECK_EQ(dump->frames[0].offset, static_cast<std::uint64_t>(0x1234));
    CHECK_EQ(dump->frames[0].build_id, std::string{"aabbccdd"});
    CHECK_EQ(dump->frames[1].module, std::string{"main"});
    CHECK_EQ(dump->frames[1].offset, static_cast<std::uint64_t>(0x40));
    CHECK(dump->frames[1].build_id.empty());
}

TEST_CASE("native_crash", "parse_dump tolerates a truncated tail") {
    // A crash while writing frame 2 leaves a partial last line; the facts
    // written before it must still parse (best-effort, header present).
    const std::string text =
        "tombstone-native-crash 1\n"
        "signal 6\n"
        "frame libc.so 0x99 dead";  // no trailing newline, no "end"
    const auto dump = nc::parse_dump(text);
    CHECK(dump.has_value());
    CHECK_EQ(dump->signal_no, 6);
    CHECK_EQ(dump->frames.size(), static_cast<std::size_t>(1));
    CHECK_EQ(dump->frames[0].module, std::string{"libc.so"});
}

TEST_CASE("native_crash", "parse_dump rejects a file without the magic header") {
    CHECK(!nc::parse_dump("signal 11\nframe x 0x1\n").has_value());
    CHECK(!nc::parse_dump("").has_value());
    CHECK(!nc::parse_dump("garbage\n").has_value());
}

TEST_CASE("native_crash", "signal_name maps the common signals") {
    CHECK_EQ(nc::signal_name(11), std::string{"SIGSEGV"});
    CHECK_EQ(nc::signal_name(6), std::string{"SIGABRT"});
    CHECK_EQ(nc::signal_name(4), std::string{"SIGILL"});
    CHECK_EQ(nc::signal_name(999), std::string{"signal 999"});
}

TEST_CASE("native_crash", "take_previous reads then deletes the dump exactly once") {
    ttest::TempDir dir;
    SdkLog log;
    const fs::path dump_path = dir.path() / nc::kDumpFileName;
    {
        std::ofstream out(dump_path, std::ios::binary);
        out << "tombstone-native-crash 1\nsignal 11\nframe app 0x10 \nend\n";
    }
    const auto first = nc::take_previous(dir.path(), log);
    CHECK(first.has_value());
    CHECK_EQ(first->signal_no, 11);
    CHECK(!fs::exists(dump_path));  // consumed
    // Second pickup on the same dir yields nothing — a dump reports once.
    CHECK(!nc::take_previous(dir.path(), log).has_value());
}

TEST_CASE("native_crash", "take_previous on a corrupt dump still deletes it (no re-report loop)") {
    ttest::TempDir dir;
    SdkLog log;
    const fs::path dump_path = dir.path() / nc::kDumpFileName;
    {
        std::ofstream out(dump_path, std::ios::binary);
        out << "not a tombstone dump at all\n";
    }
    CHECK(!nc::take_previous(dir.path(), log).has_value());  // unparsable
    CHECK(!fs::exists(dump_path));                            // but removed
}

// The crash report carries the enriched fields end-to-end on the wire.
TEST_CASE("native_crash", "build_crash_json serializes crashType + osSignal, omits when unset") {
    tombstone::CrashPayload p;
    p.occurred_at_iso = "2026-07-20T00:00:00.000Z";
    p.build_version = "1.0.0";
    p.os = "other";
    p.arch = "arm64";
    p.signature = "abc123";
    p.stack_hint = "Native crash: SIGSEGV at 0x0";
    p.crash_type = "native_crash";
    p.os_signal = 11;
    const std::string json = tombstone::build_crash_json(p);
    CHECK(json.find("\"crashType\":\"native_crash\"") != std::string::npos);
    CHECK(json.find("\"osSignal\":11") != std::string::npos);

    tombstone::CrashPayload managed;
    managed.occurred_at_iso = "2026-07-20T00:00:00.000Z";
    managed.build_version = "1.0.0";
    managed.os = "windows";
    managed.arch = "x64";
    managed.signature = "def456";
    managed.stack_hint = "NullReferenceException";
    const std::string managed_json = tombstone::build_crash_json(managed);
    CHECK(managed_json.find("crashType") == std::string::npos);
    CHECK(managed_json.find("osSignal") == std::string::npos);
}

// --------------------------------------------------------------------------
// Live capture + CHAINING integration (Linux only — the platform where CI can
// actually fault a child and inspect it). Proves, in one real SIGSEGV:
//   1. our handler wrote a parseable dump,
//   2. the PRE-EXISTING handler (Unity/UGS analog) still ran (chaining), and
//   3. the process still died with the true signal.
// --------------------------------------------------------------------------
#if defined(__linux__)
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
// A volatile null the optimizer can't prove is null at the store site, so the
// deliberate fault below isn't rejected by -Wnull-dereference under -Werror.
volatile std::uintptr_t g_fault_addr = 0;
const char *g_prev_sentinel = nullptr;
void prev_handler(int /*sig*/, siginfo_t * /*info*/, void * /*uc*/) {
    // Async-signal-safe: just create a sentinel file, then return so OUR
    // handler's reraise-default path takes the process down.
    if (g_prev_sentinel != nullptr) {
        const int fd = open(g_prev_sentinel, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            const char c = '1';
            (void)!write(fd, &c, 1);
            close(fd);
        }
    }
}
}  // namespace

TEST_CASE("native_crash", "handler dumps AND chains to a pre-existing handler, dies with the signal") {
    ttest::TempDir dir;
    const fs::path dump_path = dir.path() / nc::kDumpFileName;
    const fs::path prev_path = dir.path() / "prev-ran";
    static std::string prev_str = prev_path.string();
    g_prev_sentinel = prev_str.c_str();

    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        // CHILD: install a co-resident handler FIRST (so our install() saves it
        // for chaining), then install ours, then fault.
        struct sigaction prev;
        std::memset(&prev, 0, sizeof(prev));
        prev.sa_sigaction = prev_handler;
        sigemptyset(&prev.sa_mask);
        prev.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &prev, nullptr);

        SdkLog log;
        if (!nc::install(dir.path(), log)) _exit(42);  // unexpected on Linux

        // Deliberate fault -> SIGSEGV (g_fault_addr is a volatile 0 the compiler
        // can't fold, so this isn't flagged as a static null deref under -Werror).
        *reinterpret_cast<volatile int *>(g_fault_addr) = 5;
        _exit(0);    // unreachable
    }

    // PARENT: reap and assert.
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status));
    CHECK_EQ(WTERMSIG(status), SIGSEGV);       // died with the TRUE signal
    CHECK(fs::exists(prev_path));              // the chained handler ran
    CHECK(fs::exists(dump_path));              // our dump was written

    std::ifstream in(dump_path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto dump = nc::parse_dump(text);
    CHECK(dump.has_value());
    CHECK_EQ(dump->signal_no, SIGSEGV);
    CHECK(!dump->frames.empty());              // at least the PC frame captured
}
#endif  // __linux__
