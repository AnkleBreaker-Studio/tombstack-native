#ifndef TOMBSTONE_SRC_DEDUPE_H
#define TOMBSTONE_SRC_DEDUPE_H

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace tombstone {

/**
 * Per-signature crash dedupe: the same signature reports at most once per
 * window; repeats are suppressed (the caller turns the count into a
 * breadcrumb). The map is bounded — at capacity it resets, which at worst
 * re-allows one early report per signature and never drops a new crash.
 * Thread-safe.
 */
class DedupeWindow {
public:
    static constexpr std::chrono::seconds default_window{60};
    static constexpr std::size_t max_tracked_signatures = 64;

    struct Decision {
        bool allowed;
        /** Repeats suppressed since the last allowed report (valid when !allowed). */
        int suppressed_count;
    };

    explicit DedupeWindow(std::chrono::seconds window = default_window);

    /** Decide whether `signature` may report at `now` (steady clock). */
    Decision check(const std::string &signature, std::chrono::steady_clock::time_point now);

private:
    struct Entry {
        std::chrono::steady_clock::time_point last_sent;
        int suppressed{0};
    };

    std::mutex mutex_;
    std::unordered_map<std::string, Entry> recent_;
    std::chrono::seconds window_;
};

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_DEDUPE_H
