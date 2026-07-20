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

#include <cstdio>
#include <cstring>
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
#include <ucontext.h>
#include <unistd.h>
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
    volatile sig_atomic_t in_handler{0};  // re-entrance / double-fault guard
};

HandlerState g_state;

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
        if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 &&
            std::memcmp(name, "GNU", 3) == 0) {
            std::size_t len = note->n_descsz;
            if (len > kMaxBuildIdBytes) len = kMaxBuildIdBytes;
            std::memcpy(entry.build_id, desc, len);
            entry.build_id_len = len;
            return;
        }
        note = reinterpret_cast<const ElfW(Nhdr) *>(desc + ((note->n_descsz + 3) & ~3u));
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

/** Invoke the previously-installed handler for `sig` (chaining), supporting
 *  both SA_SIGINFO 3-arg and plain 1-arg forms, and SIG_DFL/SIG_IGN. */
void chain_old(int sig, siginfo_t *info, void *ucontext) {
    const struct sigaction *old = nullptr;
    for (int i = 0; i < kSignalCount; ++i) {
        if (kSignals[i] == sig) {
            old = &g_state.old_actions[i];
            break;
        }
    }
    if (old == nullptr) {
        reraise_default(sig);
        return;
    }
    if ((old->sa_flags & SA_SIGINFO) != 0 && old->sa_sigaction != nullptr) {
        old->sa_sigaction(sig, info, ucontext);
        return;
    }
    if (old->sa_handler == SIG_DFL || old->sa_handler == SIG_IGN || old->sa_handler == nullptr) {
        reraise_default(sig);
        return;
    }
    old->sa_handler(sig);
}

extern "C" void tombstone_signal_handler(int sig, siginfo_t *info, void *ucontext) {
    // Re-entrance / double-fault guard: if a second fatal signal arrives while
    // we're dumping, don't recurse — hand straight to the old handler.
    if (g_state.in_handler != 0) {
        chain_old(sig, info, ucontext);
        return;
    }
    g_state.in_handler = 1;

    const int fd = g_state.dump_fd;
    if (fd >= 0) {
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

        std::uintptr_t pc = 0;
        std::uintptr_t lr = 0;
        extract_pc_lr(ucontext, pc, lr);
        if (pc != 0) write_frame(fd, pc);
        if (lr != 0) write_frame(fd, lr);
        write_str(fd, "end\n");
        fsync(fd);
    }

    // Hand off to whatever was installed before us (Unity / UGS), then ensure
    // the process still dies with the real signal if that handler returns.
    chain_old(sig, info, ucontext);
    reraise_default(sig);
}

}  // namespace

bool install(const std::filesystem::path &data_dir, SdkLog &sdk_log) {
    if (g_state.installed) return true;

    // 1) Pre-open the dump fd (truncating): the handler only write()s to it.
    const std::filesystem::path dump_path = data_dir / kDumpFileName;
    g_state.dump_fd = open(dump_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (g_state.dump_fd < 0) {
        sdk_log.warn("native crash: could not open dump file; handler not installed");
        return false;
    }

    // 2) Pre-capture the loaded-module table (base/end/name/build-id).
    g_state.module_count = 0;
    dl_iterate_phdr(phdr_callback, &g_state);

    // 3) Register the pre-allocated alternate signal stack so a stack-overflow
    //    SIGSEGV still has room to run the handler (SA_ONSTACK).
    stack_t ss;
    std::memset(&ss, 0, sizeof(ss));
    ss.ss_sp = g_alt_stack;
    ss.ss_size = sizeof(g_alt_stack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

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
    sdk_log.info("native crash handler installed (experimental)");
    return true;
}

#else  // non-POSIX: capture unsupported, keep the heuristic.

bool install(const std::filesystem::path & /*data_dir*/, SdkLog &sdk_log) {
    sdk_log.info("native crash handler not supported on this platform (using heuristic)");
    return false;
}

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
        std::FILE *f = std::fopen(dump_path.string().c_str(), "rb");
        if (f != nullptr) {
            std::string text;
            char buf[4096];
            std::size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            std::fclose(f);
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
