// _GNU_SOURCE must precede every system header: it unlocks dl_iterate_phdr /
// struct dl_phdr_info in <link.h> and the REG_* ucontext indices in <ucontext.h>
// on glibc (both are __USE_GNU-guarded). No effect on non-glibc platforms.
#if defined(__linux__) || defined(__ANDROID__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif

#include "native_crash.h"

#include "sdk_log.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

// ---------------------------------------------------------------------------
// Platform gate. The live capture path is POSIX/ELF only (Linux + Android);
// everywhere else only the pure parser + next-launch reader compile, and
// install() is a no-op returning false so callers keep the heuristic.
// ---------------------------------------------------------------------------
#if defined(__linux__) || defined(__ANDROID__)
#define TOMBSTONE_NATIVE_CRASH_POSIX 1
#else
#define TOMBSTONE_NATIVE_CRASH_POSIX 0
#endif

#if TOMBSTONE_NATIVE_CRASH_POSIX
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#endif

namespace tombstone {
namespace native_crash {

namespace {

/** Write a signed decimal int into `buf`; returns the length. Pure — used by
 *  signal_name on every platform (no libc signal macros involved). */
std::size_t dec_to(long value, char *buf) {
    std::size_t n = 0;
    if (value < 0) {
        buf[n++] = '-';
        value = -value;
    }
    char tmp[24];
    std::size_t t = 0;
    do {
        tmp[t++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    while (t > 0) buf[n++] = tmp[--t];
    return n;
}

}  // namespace

std::string signal_name(int signal_no) {
    // Literal POSIX signal numbers (stable across archs): the dump stores the
    // raw number captured on the device, so this maps identically on every
    // host that reads it back — no dependency on host <signal.h> macros (which
    // don't even define SIGBUS/SIGTRAP on Windows).
    switch (signal_no) {
        case 4: return "SIGILL";
        case 5: return "SIGTRAP";
        case 6: return "SIGABRT";
        case 7: return "SIGBUS";
        case 8: return "SIGFPE";
        case 11: return "SIGSEGV";
        default: {
            char buf[24];
            const std::size_t n = dec_to(signal_no, buf);
            return "signal " + std::string(buf, n);
        }
    }
}

ChainVerdict verdict_after_chain(const ChainFacts &facts) noexcept {
    if (facts.signal_no == 6) {
        // SIGABRT (literal POSIX number): abort() terminates the process once the
        // handler returns, whatever the handler did — keep the dump.
        return ChainVerdict::die;
    }
    if (facts.context_changed) {
        // A runtime moved execution on (Mono / CoreCLR managed exception): handled,
        // even if it also re-registered its own disposition.
        return ChainVerdict::resume;
    }
    if (!facts.still_registered) {
        // The previous handler replaced our disposition (typically SIG_DFL, so the
        // re-executed fault ends the process): keep the dump and let that act.
        return ChainVerdict::defer;
    }
    if (facts.resent) {
        // It re-sent the signal to end the process by it: keep the dump and die.
        return ChainVerdict::die;
    }
    // Returned with the context unchanged: fixed in place (a GC unprotecting a page)
    // and the instruction is retried — unless the same fault keeps coming back.
    return facts.unchanged_repeats >= kMaxUnchangedRepeats ? ChainVerdict::die
                                                           : ChainVerdict::resume;
}

unsigned note_unchanged_fault(RepeatTracker &tracker, int signal_no, std::uint64_t pc,
                              std::uint64_t fault_addr, std::int64_t entered_ns) noexcept {
    const bool same_fault = tracker.count > 0 && tracker.signal_no == signal_no &&
                            tracker.pc == pc && tracker.fault_addr == fault_addr &&
                            entered_ns - tracker.last_exit_ns <= kRepeatWindowNs;
    tracker.count = same_fault ? tracker.count + 1 : 1;
    tracker.signal_no = signal_no;
    tracker.pc = pc;
    tracker.fault_addr = fault_addr;
    return tracker.count;
}

#if TOMBSTONE_NATIVE_CRASH_POSIX

namespace {

// The six process-fatal signals a game crash arrives as. SIGABRT covers a
// failed assert / std::terminate / __stack_chk_fail; the rest are hardware.
constexpr int kSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE, SIGTRAP};
constexpr int kSignalCount = static_cast<int>(sizeof(kSignals) / sizeof(kSignals[0]));

constexpr int kMaxModules = 512;
constexpr std::size_t kMaxBuildIdBytes = 20;   // SHA-1 GNU build id
constexpr std::size_t kMaxNameLen = 255;

/** Write `value` as lowercase hex ("0x...") into `buf`; returns the length.
 *  Pure arithmetic — usable from the signal handler. */
std::size_t hex_to(std::uint64_t value, char *buf) {
    buf[0] = '0';
    buf[1] = 'x';
    std::size_t n = 2;
    int shift = 60;
    while (shift > 0 && ((value >> shift) & 0xF) == 0) shift -= 4;
    for (; shift >= 0; shift -= 4) {
        const int nibble = static_cast<int>((value >> shift) & 0xF);
        buf[n++] = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
    }
    return n;
}

/** One pre-captured loaded module (filled at install time only). */
struct ModuleEntry {
    std::uintptr_t base{0};
    std::uintptr_t end{0};
    char name[kMaxNameLen + 1]{};
    unsigned char build_id[kMaxBuildIdBytes]{};
    std::size_t build_id_len{0};
};

// --- process-wide handler state. Every field is set at install time and only
//     READ from the handler (plus the atomic guard). No allocation ever. -----
struct HandlerState {
    ModuleEntry modules[kMaxModules];
    int module_count{0};
    int dump_fd{-1};
    struct sigaction old_actions[kSignalCount];
    bool installed{false};
};

HandlerState g_state;

// Re-entrance / double-fault guard. `atomic_flag::test_and_set` is the ONE lock-free op guaranteed
// async-signal-safe AND atomic across threads (a plain `volatile sig_atomic_t` read-then-write is
// not a CAS — two threads faulting at once could both pass and corrupt the single dump fd).
std::atomic_flag g_in_handler = ATOMIC_FLAG_INIT;

// Consent gate for dump writing (set_capture_enabled). Read in the handler: a lock-free atomic
// load is async-signal-safe.
std::atomic<bool> g_capture_enabled{false};

// Faults a previous handler returned from without changing the context. Only touched by the
// thread that holds g_in_handler, so plain fields are enough.
RepeatTracker g_repeats;

// Identity (device, inode) of the dump file behind g_state.dump_fd, recorded at install. The
// handler writes to or truncates the descriptor only while it still refers to that file: a host
// that closed it and reused the number must never get an unrelated file overwritten.
std::atomic<std::uintptr_t> g_dump_dev{0};
std::atomic<std::uintptr_t> g_dump_ino{0};
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free,
              "the crash handler reads the dump identity; it must be lock-free");

/** Record which file `fd` refers to (normal context, at install). */
void remember_dump_identity(int fd) {
    struct stat st;
    if (fstat(fd, &st) == 0) {
        g_dump_dev.store(static_cast<std::uintptr_t>(st.st_dev), std::memory_order_relaxed);
        g_dump_ino.store(static_cast<std::uintptr_t>(st.st_ino), std::memory_order_relaxed);
    }
}

/** True while `fd` still refers to the dump file opened at install (fstat is signal-safe). */
bool dump_fd_is_ours(int fd) {
    struct stat st;
    return fd >= 0 && fstat(fd, &st) == 0 &&
           static_cast<std::uintptr_t>(st.st_dev) == g_dump_dev.load(std::memory_order_relaxed) &&
           static_cast<std::uintptr_t>(st.st_ino) == g_dump_ino.load(std::memory_order_relaxed);
}
static_assert(std::atomic<bool>::is_always_lock_free,
              "the crash handler reads this flag; it must be lock-free");

// Fixed alternate signal stack (glibc 2.34+ made SIGSTKSZ a runtime value that
// can't size an array): 64 KiB is comfortably above SIGSTKSZ on every target.
char g_alt_stack[65536];

// -------- install-time module table (dl_iterate_phdr callback) -------------

void copy_bounded(char *dst, const char *src, std::size_t cap) {
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    std::size_t i = 0;
    for (; i < cap && src[i] != '\0'; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

void read_build_id(const ElfW(Phdr) & phdr, std::uintptr_t base, ModuleEntry &entry) {
    const auto *note = reinterpret_cast<const ElfW(Nhdr) *>(base + phdr.p_vaddr);
    const auto *end = reinterpret_cast<const char *>(note) + phdr.p_memsz;
    while (reinterpret_cast<const char *>(note) + sizeof(ElfW(Nhdr)) <= end) {
        const char *name = reinterpret_cast<const char *>(note) + sizeof(ElfW(Nhdr));
        const char *desc = name + ((note->n_namesz + 3) & ~3u);
        // Bounds: a truncated / hostile NOTE must never drive an out-of-bounds read. `desc` past the
        // segment end, or the "GNU" name / the descriptor overrunning `end`, aborts the walk.
        if (desc > end) break;
        if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 && name + 4 <= end &&
            std::memcmp(name, "GNU", 3) == 0) {
            std::size_t len = note->n_descsz;
            if (len > kMaxBuildIdBytes) len = kMaxBuildIdBytes;
            if (desc + len <= end) {
                std::memcpy(entry.build_id, desc, len);
                entry.build_id_len = len;
            }
            return;
        }
        const char *next = desc + ((note->n_descsz + 3) & ~3u);
        if (next <= reinterpret_cast<const char *>(note)) break;  // no forward progress → stop
        note = reinterpret_cast<const ElfW(Nhdr) *>(next);
    }
}

int phdr_callback(struct dl_phdr_info *info, std::size_t /*size*/, void *data) {
    auto *state = static_cast<HandlerState *>(data);
    if (state->module_count >= kMaxModules) return 0;
    ModuleEntry &entry = state->modules[state->module_count];

    const char *name = (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0')
                           ? info->dlpi_name
                           : "main";
    copy_bounded(entry.name, name, kMaxNameLen);

    std::uintptr_t lo = 0;
    std::uintptr_t hi = 0;
    bool first = true;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &phdr = info->dlpi_phdr[i];
        if (phdr.p_type == PT_LOAD) {
            const std::uintptr_t seg_lo = info->dlpi_addr + phdr.p_vaddr;
            const std::uintptr_t seg_hi = seg_lo + phdr.p_memsz;
            if (first || seg_lo < lo) lo = seg_lo;
            if (first || seg_hi > hi) hi = seg_hi;
            first = false;
        } else if (phdr.p_type == PT_NOTE) {
            read_build_id(phdr, info->dlpi_addr, entry);
        }
    }
    if (first) return 0;  // no PT_LOAD — skip
    entry.base = lo;
    entry.end = hi;
    state->module_count++;
    return 0;
}

/** Linear resolve of an absolute PC to (module index, offset). Handler-safe
 *  (no allocation). Returns -1 when the address falls in no known module. */
int resolve_module(std::uintptr_t addr, std::uint64_t &offset_out) {
    for (int i = 0; i < g_state.module_count; ++i) {
        const ModuleEntry &m = g_state.modules[i];
        if (addr >= m.base && addr < m.end) {
            offset_out = static_cast<std::uint64_t>(addr - m.base);
            return i;
        }
    }
    return -1;
}

// -------- the async-signal-safe crash handler ------------------------------

void write_all(int fd, const char *buf, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        const ssize_t w = write(fd, buf + off, len - off);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return;  // give up silently — never loop-forever in a handler
        }
        off += static_cast<std::size_t>(w);
    }
}

void write_str(int fd, const char *s) { write_all(fd, s, std::strlen(s)); }

void write_hex(int fd, std::uint64_t v) {
    char buf[20];
    write_all(fd, buf, hex_to(v, buf));
}

void write_dec(int fd, long v) {
    char buf[24];
    write_all(fd, buf, dec_to(v, buf));
}

/** Extract the instruction pointer and (where the arch has one) the link
 *  register from the signal ucontext. Values are 0 when unavailable. */
void extract_pc_lr(void *ucontext, std::uintptr_t &pc, std::uintptr_t &lr) {
    pc = 0;
    lr = 0;
    if (ucontext == nullptr) return;
    auto *uc = static_cast<ucontext_t *>(ucontext);
#if defined(__aarch64__)
    pc = static_cast<std::uintptr_t>(uc->uc_mcontext.pc);
    lr = static_cast<std::uintptr_t>(uc->uc_mcontext.regs[30]);
#elif defined(__arm__)
    pc = static_cast<std::uintptr_t>(uc->uc_mcontext.arm_pc);
    lr = static_cast<std::uintptr_t>(uc->uc_mcontext.arm_lr);
#elif defined(__x86_64__)
    pc = static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__i386__)
    pc = static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_EIP]);
#else
    (void)uc;
#endif
}

/** The stack pointer from the signal ucontext; 0 when unavailable. Read before
 *  and after chaining: a runtime that resolved the fault moved it or the PC. */
std::uintptr_t extract_sp(void *ucontext) {
    if (ucontext == nullptr) return 0;
    auto *uc = static_cast<ucontext_t *>(ucontext);
#if defined(__aarch64__)
    return static_cast<std::uintptr_t>(uc->uc_mcontext.sp);
#elif defined(__arm__)
    return static_cast<std::uintptr_t>(uc->uc_mcontext.arm_sp);
#elif defined(__x86_64__)
    return static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]);
#elif defined(__i386__)
    return static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_ESP]);
#else
    (void)uc;
    return 0;
#endif
}

void write_frame(int fd, std::uintptr_t addr) {
    std::uint64_t offset = 0;
    const int idx = resolve_module(addr, offset);
    write_str(fd, "frame ");
    if (idx < 0) {
        // Unresolved (JIT / stripped / stack) — record the absolute addr so
        // it's not silently lost; server-side it simply won't symbolicate.
        write_str(fd, "?? ");
        write_hex(fd, static_cast<std::uint64_t>(addr));
        write_str(fd, " \n");
        return;
    }
    const ModuleEntry &m = g_state.modules[idx];
    write_str(fd, m.name);
    write_str(fd, " ");
    write_hex(fd, offset);
    write_str(fd, " ");
    static const char kHex[] = "0123456789abcdef";
    char bid[kMaxBuildIdBytes * 2];
    std::size_t bn = 0;
    for (std::size_t i = 0; i < m.build_id_len; ++i) {
        bid[bn++] = kHex[(m.build_id[i] >> 4) & 0xF];
        bid[bn++] = kHex[m.build_id[i] & 0xF];
    }
    write_all(fd, bid, bn);
    write_str(fd, "\n");
}

/** Restore the default disposition for `sig` and re-raise so the process dies
 *  with the TRUE signal after we + any chained handler are done. */
void reraise_default(int sig) {
    struct sigaction dfl;
    std::memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, nullptr);
    raise(sig);
}

/** The saved previous action for `sig`; nullptr when `sig` is not one of ours. */
struct sigaction *previous_action(int sig) {
    for (int i = 0; i < kSignalCount; ++i) {
        if (kSignals[i] == sig) {
            return &g_state.old_actions[i];
        }
    }
    return nullptr;
}

/** True when `act` names a function to call rather than SIG_DFL / SIG_IGN.
 *  sa_handler and sa_sigaction share storage, so this holds for both forms. */
bool is_handler(const struct sigaction &act) {
    return act.sa_handler != SIG_DFL && act.sa_handler != SIG_IGN;
}

void invoke_previous(const struct sigaction &act, int sig, siginfo_t *info, void *ucontext) {
    if ((act.sa_flags & SA_SIGINFO) != 0) {
        act.sa_sigaction(sig, info, ucontext);
    } else {
        act.sa_handler(sig);
    }
}

/** Hand the signal to the previously-installed handler (SA_SIGINFO or plain
 *  form); with none (SIG_DFL / SIG_IGN), restore the default and re-raise. */
void chain_old(int sig, siginfo_t *info, void *ucontext) {
    const struct sigaction *old = previous_action(sig);
    if (old == nullptr || !is_handler(*old)) {
        reraise_default(sig);
        return;
    }
    invoke_previous(*old, sig, info, ucontext);
}

/** Write the fixed-format dump (no fsync: the page cache outlives the process).
 *  Returns the fd written to, or -1 when capture is off or unavailable. */
int write_dump(int sig, const siginfo_t *info, std::uintptr_t pc, std::uintptr_t lr) {
    const int fd = g_capture_enabled.load(std::memory_order_relaxed) ? g_state.dump_fd : -1;
    if (fd < 0 || !dump_fd_is_ours(fd)) return -1;
    write_str(fd, "tombstone-native-crash 1\n");
    write_str(fd, "signal ");
    write_dec(fd, sig);
    write_str(fd, "\n");
    write_str(fd, "code ");
    write_dec(fd, info != nullptr ? info->si_code : 0);
    write_str(fd, "\n");
    write_str(fd, "fault ");
    write_hex(fd, info != nullptr ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0);
    write_str(fd, "\n");
    if (pc != 0) write_frame(fd, pc);
    if (lr != 0) write_frame(fd, lr);
    write_str(fd, "end\n");
    return fd;
}

/** Withdraw a dump written for a fault that turned out to be handled. */
void retract_dump(int fd) {
    if (fd < 0 || !dump_fd_is_ours(fd)) return;
    const int truncated = ftruncate(fd, 0);
    (void)truncated;
    lseek(fd, 0, SEEK_SET);
}

std::int64_t monotonic_ns() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000000LL + static_cast<std::int64_t>(ts.tv_nsec);
}

extern "C" void tombstone_signal_handler(int sig, siginfo_t *info, void *ucontext);

/** True while our handler is still the installed disposition for `sig`. */
bool still_registered(int sig) {
    struct sigaction current;
    std::memset(&current, 0, sizeof(current));
    if (sigaction(sig, nullptr, &current) != 0) return true;  // cannot tell: assume unchanged
    return (current.sa_flags & SA_SIGINFO) != 0 && current.sa_sigaction == tombstone_signal_handler;
}

/** True when `sig` is pending again (blocked while we run): the previous handler re-sent it. */
bool signal_pending(int sig) {
    sigset_t pending;
    sigemptyset(&pending);
    if (sigpending(&pending) != 0) return false;
    return sigismember(&pending, sig) == 1;
}

extern "C" void tombstone_signal_handler(int sig, siginfo_t *info, void *ucontext) {
    // The handler may now return into the interrupted code: leave its errno untouched.
    const int saved_errno = errno;
    // Re-entrance / double-fault guard (atomic test-and-set): a second signal — including one on
    // another thread while we're dumping — must not recurse or share the fd; hand straight to the old
    // handler. Cleared again only when a fault turns out to be handled (resume below).
    if (g_in_handler.test_and_set()) {
        chain_old(sig, info, ucontext);
        errno = saved_errno;
        return;
    }
    const std::int64_t entered_ns = monotonic_ns();

    std::uintptr_t pc = 0;
    std::uintptr_t lr = 0;
    extract_pc_lr(ucontext, pc, lr);
    const std::uintptr_t sp = extract_sp(ucontext);
    const std::uintptr_t fault =
        info != nullptr ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0;
    // Written BEFORE chaining: an engine crash reporter that ends the process itself
    // (Unity / UGS) must not cost us the dump.
    const int fd = write_dump(sig, info, pc, lr);

    struct sigaction *previous = previous_action(sig);
    if (previous == nullptr || !is_handler(*previous)) {
        // Nobody else handles this signal: it is fatal, exactly as without us.
        if (fd >= 0) fsync(fd);
        reraise_default(sig);
        return;
    }
    const bool one_shot = (previous->sa_flags & SA_RESETHAND) != 0;
    invoke_previous(*previous, sig, info, ucontext);

    // The previous handler RETURNED. A one-shot (SA_RESETHAND) handler expected the
    // next delivery to take the default action: emulate that from now on.
    if (one_shot) {
        previous->sa_handler = SIG_DFL;
        previous->sa_flags = 0;
    }
    std::uintptr_t pc_after = 0;
    std::uintptr_t lr_after = 0;
    extract_pc_lr(ucontext, pc_after, lr_after);
    ChainFacts facts;
    facts.signal_no = sig;
    facts.still_registered = still_registered(sig);
    facts.context_changed = pc_after != pc || extract_sp(ucontext) != sp;
    facts.resent = signal_pending(sig);
    if (facts.context_changed) {
        g_repeats.count = 0;
    } else {
        facts.unchanged_repeats = note_unchanged_fault(g_repeats, sig, pc, fault, entered_ns);
    }
    switch (verdict_after_chain(facts)) {
    case ChainVerdict::resume:
        // Handled (a managed exception, a page fixed in place): not a crash.
        retract_dump(fd);
        g_repeats.last_exit_ns = monotonic_ns();
        g_in_handler.clear();
        errno = saved_errno;
        return;
    case ChainVerdict::defer:
        if (fd >= 0) fsync(fd);
        errno = saved_errno;
        return;
    case ChainVerdict::die:
    default:
        if (fd >= 0) fsync(fd);
        reraise_default(sig);
        errno = saved_errno;
        return;
    }
}

}  // namespace

void set_capture_enabled(bool enabled) noexcept {
    g_capture_enabled.store(enabled, std::memory_order_relaxed);
}

bool install(const std::filesystem::path &data_dir, SdkLog &sdk_log) {
    const std::filesystem::path dump_path = data_dir / kDumpFileName;
    if (g_state.installed) {
        // Initialized again in this process (after a shutdown, maybe for another data
        // directory): the previous dump file was consumed by take_previous, so point the
        // existing descriptor at a fresh one. dup2 swaps it atomically — the handler never
        // sees a closed or half-replaced descriptor.
        const int fresh = open(dump_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fresh >= 0 && dup2(fresh, g_state.dump_fd) >= 0) {
            remember_dump_identity(g_state.dump_fd);
        } else {
            sdk_log.warn("native crash: could not reopen the dump file; crashes are not dumped");
        }
        if (fresh >= 0) close(fresh);
        set_capture_enabled(true);
        return true;
    }

    // 1) Pre-open the dump fd (truncating): the handler only write()s to it.
    g_state.dump_fd = open(dump_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (g_state.dump_fd < 0) {
        sdk_log.warn("native crash: could not open dump file; handler not installed");
        return false;
    }
    remember_dump_identity(g_state.dump_fd);

    // 2) Pre-capture the loaded-module table (base/end/name/build-id).
    g_state.module_count = 0;
    dl_iterate_phdr(phdr_callback, &g_state);

    // 3) Make sure this thread has an alternate signal stack so a stack-overflow
    //    SIGSEGV still has room to run the handler (SA_ONSTACK) — but never
    //    replace one the engine or a managed runtime already registered: its own
    //    stack-overflow handling relies on it.
    stack_t existing;
    std::memset(&existing, 0, sizeof(existing));
    const bool has_alt_stack = sigaltstack(nullptr, &existing) == 0 &&
                               (existing.ss_flags & SS_DISABLE) == 0 && existing.ss_sp != nullptr;
    if (!has_alt_stack) {
        stack_t ss;
        std::memset(&ss, 0, sizeof(ss));
        ss.ss_sp = g_alt_stack;
        ss.ss_size = sizeof(g_alt_stack);
        ss.ss_flags = 0;
        sigaltstack(&ss, nullptr);
    }

    // 4) Install, saving each previous action for chaining.
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = tombstone_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    bool any = false;
    for (int i = 0; i < kSignalCount; ++i) {
        if (sigaction(kSignals[i], &sa, &g_state.old_actions[i]) == 0) {
            any = true;
        }
    }
    if (!any) {
        close(g_state.dump_fd);
        g_state.dump_fd = -1;
        sdk_log.warn("native crash: sigaction failed for all signals; not installed");
        return false;
    }
    g_state.installed = true;
    set_capture_enabled(true);
    sdk_log.info("native crash handler installed (experimental)");
    return true;
}

#else  // non-POSIX: capture unsupported, keep the heuristic.

bool install(const std::filesystem::path & /*data_dir*/, SdkLog &sdk_log) {
    sdk_log.info("native crash handler not supported on this platform (using heuristic)");
    return false;
}

void set_capture_enabled(bool /*enabled*/) noexcept {}

#endif  // TOMBSTONE_NATIVE_CRASH_POSIX

// ---------------------------------------------------------------------------
// Pure, cross-platform: dump parsing + next-launch pickup.
// ---------------------------------------------------------------------------

namespace {

std::uint64_t parse_hex(std::string_view s) {
    std::uint64_t v = 0;
    std::size_t i = 0;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) i = 2;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
        else break;
        v = (v << 4) | static_cast<std::uint64_t>(d);
    }
    return v;
}

long parse_dec(std::string_view s) {
    long v = 0;
    bool neg = false;
    std::size_t i = 0;
    if (i < s.size() && s[i] == '-') {
        neg = true;
        ++i;
    }
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) v = v * 10 + (s[i] - '0');
    return neg ? -v : v;
}

}  // namespace

std::optional<NativeCrashDump> parse_dump(std::string_view text) {
    // Tolerate a truncated tail: parse line by line, keep what's valid, only
    // require the magic header to accept the dump at all.
    NativeCrashDump dump;
    bool saw_header = false;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string_view line =
            text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() : nl + 1;
        if (line.empty()) continue;

        auto starts_with = [&](std::string_view p) {
            return line.size() >= p.size() && line.substr(0, p.size()) == p;
        };
        if (starts_with("tombstone-native-crash")) {
            saw_header = true;
        } else if (starts_with("signal ")) {
            dump.signal_no = static_cast<int>(parse_dec(line.substr(7)));
        } else if (starts_with("code ")) {
            dump.code = static_cast<int>(parse_dec(line.substr(5)));
        } else if (starts_with("fault ")) {
            dump.fault_addr = parse_hex(line.substr(6));
        } else if (starts_with("frame ")) {
            // "frame <module> <offset-hex> [build-id-hex]" or "frame ?? <addr>"
            const std::string_view rest = line.substr(6);
            const std::size_t sp1 = rest.find(' ');
            if (sp1 == std::string_view::npos) continue;
            NativeCrashFrame frame;
            frame.module = std::string(rest.substr(0, sp1));
            const std::string_view after = rest.substr(sp1 + 1);
            const std::size_t sp2 = after.find(' ');
            frame.offset = parse_hex(after.substr(0, sp2));
            if (sp2 != std::string_view::npos) {
                std::string_view bid = after.substr(sp2 + 1);
                while (!bid.empty() && bid.back() == ' ') bid.remove_suffix(1);
                frame.build_id = std::string(bid);
            }
            dump.frames.push_back(std::move(frame));
        }
        // "end" and anything unknown: ignored.
    }
    if (!saw_header) return std::nullopt;
    return dump;
}

std::optional<NativeCrashDump> take_previous(const std::filesystem::path &data_dir, SdkLog &sdk_log) {
    const std::filesystem::path dump_path = data_dir / kDumpFileName;
    std::error_code ec;
    if (!std::filesystem::exists(dump_path, ec)) return std::nullopt;
    std::optional<NativeCrashDump> result;
    try {
        // Opened through the path itself (wide on Windows), never a narrowed copy.
        std::ifstream in{dump_path, std::ios::binary};
        if (in) {
            const std::string text{std::istreambuf_iterator<char>{in},
                                   std::istreambuf_iterator<char>{}};
            result = parse_dump(text);
        }
    } catch (const std::exception &e) {
        sdk_log.warn(std::string{"native crash: dump read failed: "} + e.what());
    }
    // Always delete: a dump is consumed exactly once, even if unparsable, so a
    // corrupt file can never re-report every launch.
    std::filesystem::remove(dump_path, ec);
    return result;
}

}  // namespace native_crash
}  // namespace tombstone
