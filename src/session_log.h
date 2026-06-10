#ifndef TOMBSTONE_SRC_SESSION_LOG_H
#define TOMBSTONE_SRC_SESSION_LOG_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace tombstone {

class SdkLog;

/**
 * Bounded rolling session log: lines buffer in memory (any thread) and flush
 * to `<data_dir>/session.log` — periodically from the worker thread, plus a
 * synchronous flush on the crash path and at shutdown. The file is capped at
 * 512 KB with a truncate-from-front trim (newest lines win). At init the
 * previous run's file rotates to `previous-session.log` so an unclean
 * shutdown's log survives for next-launch upload. Fail-silent: filesystem
 * errors degrade to a single warning through SdkLog.
 */
class SessionLog {
public:
    static constexpr std::size_t max_log_bytes = 512 * 1024;
    static constexpr std::size_t trimmed_log_bytes = max_log_bytes / 2;
    static constexpr std::size_t max_pending_bytes = 64 * 1024;

    explicit SessionLog(SdkLog &sdk_log) noexcept;

    /** Cache paths. Must run before any other call. */
    void configure(const std::filesystem::path &data_dir);

    /**
     * Rotate `session.log` -> `previous-session.log` (deleting any stale
     * previous log) and reset for this session. Returns true when a
     * previous-session log exists after rotation.
     */
    bool rotate_for_new_session();

    /** Buffer one line: "<tsIso> [<level>] <message>". Drops when the buffer is full. */
    void append(std::string_view ts_iso, std::string_view level, std::string_view message);

    /** Synchronously write the pending buffer to disk (crash path / shutdown). */
    void flush_now();

    /** Current session log bytes (flushed first). Empty when unavailable. */
    std::vector<char> read_current();

    /** Preserved previous-session log bytes. Empty when unavailable. */
    std::vector<char> read_previous();

private:
    std::string take_pending();
    void write_chunk(const std::string &chunk);
    void trim_from_front();
    std::vector<char> read_file(const std::filesystem::path &path);

    SdkLog &sdk_log_;
    std::mutex pending_mutex_;
    std::string pending_;
    std::size_t dropped_lines_{0};

    std::mutex file_mutex_;
    std::filesystem::path current_path_;
    std::filesystem::path previous_path_;
    std::uintmax_t file_bytes_{0};
    bool configured_{false};
};

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_SESSION_LOG_H
