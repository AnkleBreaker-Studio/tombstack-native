#ifndef TOMBSTONE_SRC_NATIVE_CRASH_H
#define TOMBSTONE_SRC_NATIVE_CRASH_H

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tombstone {

class SdkLog;

/** One resolved native frame: the module it fell in and the module-relative
 *  offset (what addr2line / server-side symbolication takes), plus the ELF
 *  GNU build id when the module carries one (empty = unknown). */
struct NativeCrashFrame {
    std::string module;
    std::uint64_t offset{0};
    std::string build_id;
};

/** A parsed native crash dump from a previous run: the raw signal facts plus
 *  the captured frames (frame 0 = PC, frame 1 = LR where the arch has one). */
struct NativeCrashDump {
    int signal_no{0};
    int code{0};
    std::uint64_t fault_addr{0};
    std::vector<NativeCrashFrame> frames;
};

/**
 * In-process native crash capture (v0.9, EXPERIMENTAL, default-off).
 *
 * Design rules (the async-signal-safety contract):
 *  - EVERYTHING expensive happens at install time, in a normal context: the
 *    module table (base/end/name/build-id via dl_iterate_phdr + PT_NOTE) is
 *    pre-captured, the dump fd is pre-opened, the alt stack pre-allocated.
 *  - The HANDLER does arithmetic and write() only: re-entrance guard, PC/LR
 *    from ucontext, linear module resolve, fixed-format dump, fsync — no
 *    malloc, no locks, no printf, no managed callbacks.
 *  - CHAINING: the previous sigaction is saved and invoked after the dump is
 *    written (SA_SIGINFO and plain handlers both supported), so Unity / UGS
 *    Cloud Diagnostics style co-resident handlers still run. With no previous
 *    handler (SIG_DFL / SIG_IGN) the signal is fatal: the default action is
 *    restored and the signal re-raised so the process dies with the true
 *    signal. When a previous handler RETURNS it may have resolved the fault —
 *    a managed runtime (Mono, CoreCLR) turns a null dereference into an
 *    exception by moving the PC, a GC fixes a protected page and retries — so
 *    verdict_after_chain() decides: resume (the dump is withdrawn) or die.
 *  - An alternate signal stack already registered on the installing thread is
 *    kept; one is registered only when none exists.
 *  - NO on-device symbolication: frames are (module, build-id, offset) only;
 *    symbolication happens server-side against uploaded symbols.
 *
 * Platform scope v1: Linux + Android (ELF/POSIX). install() returns false on
 * other platforms (and on any setup failure) — callers degrade gracefully to
 * the existing unclean-shutdown heuristic. Windows SEH and iOS/Mach are the
 * tracked follow-ups.
 */
namespace native_crash {

/** Dump file name inside the SDK data dir (sibling of session.lock). */
inline constexpr const char *kDumpFileName = "native-crash.dump";

/**
 * Read AND delete a surviving dump from the previous run (next-launch pickup,
 * normal context, any platform). nullopt = no dump / unreadable / unparsable.
 */
std::optional<NativeCrashDump> take_previous(const std::filesystem::path &data_dir, SdkLog &sdk_log);

/**
 * Install the signal handlers (SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE/SIGTRAP)
 * and enable dump writing. Call from a normal context, AFTER take_previous (the
 * dump file is truncated on the first call); later calls only re-enable dump
 * writing. Returns false on unsupported platforms or setup failure — never
 * throws into game code.
 */
bool install(const std::filesystem::path &data_dir, SdkLog &sdk_log);

/**
 * Enable or disable dump writing (consent). While disabled, an installed
 * handler writes nothing but still chains to the previous handler exactly as
 * before. A no-op where capture is unsupported. Safe from any thread.
 */
void set_capture_enabled(bool enabled) noexcept;

/** Pure, host-testable: parse the dump text (tolerates a truncated tail —
 *  a crash mid-write must still yield the facts written so far). */
std::optional<NativeCrashDump> parse_dump(std::string_view text);

/** Pure: "SIGSEGV" for 11 etc.; "signal <n>" when unknown. */
std::string signal_name(int signal_no);

/** What the handler does once the previously installed handler has RETURNED. */
enum class ChainVerdict {
    /** The fault was resolved: withdraw the dump and resume execution. */
    resume,
    /** The fault is fatal: keep the dump, restore the default action, re-raise. */
    die,
    /** The previous handler installed another disposition: keep the dump and let it act. */
    defer,
};

/** Facts about one fault, gathered after the previous handler returned. */
struct ChainFacts {
    int signal_no{0};
    /** Our handler is still the installed disposition for the signal. */
    bool still_registered{true};
    /** The previous handler moved the PC or the stack pointer (redirected execution). */
    bool context_changed{false};
    /** Identical faults in a row that the previous handler returned from unchanged (this one included). */
    unsigned unchanged_repeats{0};
};

/** A fault the previous handler keeps returning from unchanged is treated as unresolved
 *  once it has recurred this many times in a row (each within kRepeatWindowNs). */
inline constexpr unsigned kMaxUnchangedRepeats = 16;
inline constexpr std::int64_t kRepeatWindowNs = 1000000000;  // 1 s

/** Pure (host-testable): the handler's decision after the previous handler returned. */
ChainVerdict verdict_after_chain(const ChainFacts &facts) noexcept;

/** Identical faults (same signal, PC and fault address) returned from unchanged, in a row. */
struct RepeatTracker {
    int signal_no{0};
    std::uint64_t pc{0};
    std::uint64_t fault_addr{0};
    std::int64_t last_ns{0};
    unsigned count{0};
};

/** Pure (host-testable): record one fault the previous handler returned from with an
 *  unchanged context and return how many identical ones have now come in a row (a fault
 *  more than kRepeatWindowNs after the last one starts a new run). */
unsigned note_unchanged_fault(RepeatTracker &tracker, int signal_no, std::uint64_t pc,
                              std::uint64_t fault_addr, std::int64_t now_ns) noexcept;

}  // namespace native_crash

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_NATIVE_CRASH_H
