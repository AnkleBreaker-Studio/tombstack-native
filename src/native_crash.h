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
 *  - CHAINING: the previous sigaction is saved and re-installed before the
 *    old handler is invoked (SA_SIGINFO and plain handlers both supported),
 *    so Unity / UGS Cloud Diagnostics style co-resident handlers still run;
 *    if the old handler returns, the default action is restored and the
 *    signal re-raised so the process dies with the true signal.
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
 * Install the signal handlers (SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE/SIGTRAP).
 * Call once, from a normal context, AFTER take_previous (the dump file is
 * truncated here). Returns false on unsupported platforms or setup failure —
 * never throws into game code.
 */
bool install(const std::filesystem::path &data_dir, SdkLog &sdk_log);

/** Pure, host-testable: parse the dump text (tolerates a truncated tail —
 *  a crash mid-write must still yield the facts written so far). */
std::optional<NativeCrashDump> parse_dump(std::string_view text);

/** Pure: "SIGSEGV" for 11 etc.; "signal <n>" when unknown. */
std::string signal_name(int signal_no);

}  // namespace native_crash

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_NATIVE_CRASH_H
