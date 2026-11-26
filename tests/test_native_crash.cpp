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
// After the previous handler returns: resolved faults resume, unresolved ones die.
// Pure decision logic, so it runs on every platform.
// --------------------------------------------------------------------------

TEST_CASE("native_crash", "a fault a runtime redirected or fixed in place resumes") {
    nc::ChainFacts redirected;  // Mono / CoreCLR moved the PC to a managed exception
    redirected.signal_no = 11;
    redirected.context_changed = true;
    CHECK(nc::verdict_after_chain(redirected) == nc::ChainVerdict::resume);

    nc::ChainFacts fixed;  // a GC unprotected the page and wants the store retried
    fixed.signal_no = 11;
    fixed.unchanged_repeats = 1;
    CHECK(nc::verdict_after_chain(fixed) == nc::ChainVerdict::resume);

    nc::ChainFacts divide;  // a managed DivideByZeroException
    divide.signal_no = 8;
    divide.context_changed = true;
    CHECK(nc::verdict_after_chain(divide) == nc::ChainVerdict::resume);
}

TEST_CASE("native_crash", "an unresolved, aborting or re-registered fault does not resume") {
    nc::ChainFacts looping;  // the same fault keeps coming back: nothing resolved it
    looping.signal_no = 11;
    looping.unchanged_repeats = nc::kMaxUnchangedRepeats;
    CHECK(nc::verdict_after_chain(looping) == nc::ChainVerdict::die);

    nc::ChainFacts aborted;  // abort() ends the process whatever the handler did
    aborted.signal_no = 6;
    aborted.context_changed = true;
    CHECK(nc::verdict_after_chain(aborted) == nc::ChainVerdict::die);

    nc::ChainFacts replaced;  // the previous handler installed SIG_DFL to die by it
    replaced.signal_no = 11;
    replaced.still_registered = false;
    CHECK(nc::verdict_after_chain(replaced) == nc::ChainVerdict::defer);
}

TEST_CASE("native_crash", "repeat counting needs the same fault in quick succession") {
    nc::RepeatTracker tracker;
    const std::int64_t t0 = 5'000'000'000;
    CHECK_EQ(nc::note_unchanged_fault(tracker, 11, 0x1000, 0x10, t0), 1U);
    CHECK_EQ(nc::note_unchanged_fault(tracker, 11, 0x1000, 0x10, t0 + 1000), 2U);
    CHECK_EQ(nc::note_unchanged_fault(tracker, 11, 0x1000, 0x10, t0 + 2000), 3U);
    // Another faulting instruction, address or signal starts a new run.
    CHECK_EQ(nc::note_unchanged_fault(tracker, 11, 0x2000, 0x10, t0 + 3000), 1U);
    CHECK_EQ(nc::note_unchanged_fault(tracker, 11, 0x2000, 0x20, t0 + 4000), 1U);
    CHECK_EQ(nc::note_unchanged_fault(tracker, 7, 0x2000, 0x20, t0 + 5000), 1U);
    // So does the same fault long after the last one (a page protected again later).
    CHECK_EQ(nc::note_unchanged_fault(tracker, 7, 0x2000, 0x20, t0 + 6000), 2U);
    CHECK_EQ(nc::note_unchanged_fault(tracker, 7, 0x2000, 0x20,
                                      t0 + 6000 + nc::kRepeatWindowNs + 1),
             1U);
}

// --------------------------------------------------------------------------
// Live capture + CHAINING integration (Linux only — the platform where CI can
// actually fault a child and inspect it). Proves, with real faults:
//   1. our handler writes a parseable dump and the PRE-EXISTING handler
//      (Unity/UGS analog) still runs (chaining);
//   2. a fault nothing resolves still kills the process with the true signal;
//   3. a fault the previous handler resolves (moving the PC like a managed
//      runtime, or fixing the page like a GC) is resumed and leaves no dump;
//   4. an alternate signal stack registered before install is kept;
//   5. with capture disabled (consent), no dump is written.
// --------------------------------------------------------------------------
#if defined(__linux__)
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

namespace {
// A volatile null the optimizer can't prove is null at the store site, so the
// deliberate fault below isn't rejected by -Wnull-dereference under -Werror.
volatile std::uintptr_t g_fault_addr = 0;
const char *g_prev_sentinel = nullptr;
void prev_handler(int /*sig*/, siginfo_t * /*info*/, void * /*uc*/) {
    // Async-signal-safe: just create a sentinel file, then return WITHOUT
    // resolving the fault, so it recurs until OUR handler declares it fatal.
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

namespace {

// A GC-style previous handler: resolves the fault in place (unprotects the page)
// and returns, so the faulting store is retried and succeeds.
void *g_guard_page = nullptr;
std::size_t g_page_size = 0;
void unprotect_handler(int /*sig*/, siginfo_t * /*info*/, void * /*uc*/) {
    mprotect(g_guard_page, g_page_size, PROT_READ | PROT_WRITE);
}

#if defined(__x86_64__) || defined(__aarch64__)
#if defined(__x86_64__)
#define TOMBSTONE_TEST_REALIGN __attribute__((force_align_arg_pointer))
#else
#define TOMBSTONE_TEST_REALIGN
#endif

// A managed-runtime-style previous handler (Mono / CoreCLR): turns the fault into
// an "exception" by moving the PC to recovery code, then returns.
[[noreturn]] TOMBSTONE_TEST_REALIGN void recovered_from_fault() { _exit(0); }

void redirect_handler(int /*sig*/, siginfo_t * /*info*/, void *ucontext) {
    auto *uc = static_cast<ucontext_t *>(ucontext);
    const auto target = reinterpret_cast<std::uintptr_t>(&recovered_from_fault);
#if defined(__x86_64__)
    uc->uc_mcontext.gregs[REG_RIP] = static_cast<greg_t>(target);
#else
    uc->uc_mcontext.pc = target;
#endif
}
#endif

void install_previous(int sig, void (*handler)(int, siginfo_t *, void *)) {
    struct sigaction prev;
    std::memset(&prev, 0, sizeof(prev));
    prev.sa_sigaction = handler;
    sigemptyset(&prev.sa_mask);
    prev.sa_flags = SA_SIGINFO;
    sigaction(sig, &prev, nullptr);
}

/** Run `child` in a forked child (it must _exit itself); returns the wait status. */
template <typename Child>
int run_in_child(Child child) {
    const pid_t pid = fork();
    if (pid == 0) {
        child();
        _exit(99);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}

}  // namespace

TEST_CASE("native_crash", "a fault the previous handler fixes in place is resumed, not fatal") {
    ttest::TempDir dir;
    const fs::path base = dir.path();
    const int status = run_in_child([&] {
        g_page_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        g_guard_page = mmap(nullptr, g_page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_guard_page == MAP_FAILED) _exit(40);
        install_previous(SIGSEGV, unprotect_handler);
        SdkLog log;
        if (!nc::install(base, log)) _exit(42);
        volatile int *slot = static_cast<volatile int *>(g_guard_page);
        *slot = 7;  // faults once; the previous handler unprotects the page; the store retries
        _exit(*slot == 7 ? 0 : 41);
    });
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);
    SdkLog log;
    CHECK(!nc::take_previous(base, log).has_value());  // the handled fault left no dump
}

#if defined(__x86_64__) || defined(__aarch64__)
TEST_CASE("native_crash", "a fault a managed runtime turns into an exception does not kill") {
    ttest::TempDir dir;
    const fs::path base = dir.path();
    const int status = run_in_child([&] {
        install_previous(SIGSEGV, redirect_handler);
        SdkLog log;
        if (!nc::install(base, log)) _exit(42);
        *reinterpret_cast<volatile int *>(g_fault_addr) = 5;  // a "NullReferenceException"
        _exit(43);  // not reached: execution continues in recovered_from_fault()
    });
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);
    SdkLog log;
    CHECK(!nc::take_previous(base, log).has_value());
}
#endif

TEST_CASE("native_crash", "an alternate signal stack registered before install is kept") {
    ttest::TempDir dir;
    const fs::path base = dir.path();
    const int status = run_in_child([&] {
        static char engine_stack[64 * 1024];
        stack_t mine;
        std::memset(&mine, 0, sizeof(mine));
        mine.ss_sp = engine_stack;
        mine.ss_size = sizeof(engine_stack);
        if (sigaltstack(&mine, nullptr) != 0) _exit(40);
        SdkLog log;
        if (!nc::install(base, log)) _exit(42);
        stack_t now;
        std::memset(&now, 0, sizeof(now));
        sigaltstack(nullptr, &now);
        _exit(now.ss_sp == static_cast<void *>(engine_stack) ? 0 : 1);
    });
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);
}

TEST_CASE("native_crash", "with capture disabled no dump is written and the crash still happens") {
    ttest::TempDir dir;
    const fs::path base = dir.path();
    const int status = run_in_child([&] {
        SdkLog log;
        if (!nc::install(base, log)) _exit(42);
        nc::set_capture_enabled(false);  // consent withdrawn
        *reinterpret_cast<volatile int *>(g_fault_addr) = 5;
        _exit(0);
    });
    CHECK(WIFSIGNALED(status));
    CHECK_EQ(WTERMSIG(status), SIGSEGV);
    SdkLog log;
    CHECK(!nc::take_previous(base, log).has_value());
}
#endif  // __linux__
