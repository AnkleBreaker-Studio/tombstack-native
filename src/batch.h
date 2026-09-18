#ifndef TOMBSTONE_SRC_BATCH_H
#define TOMBSTONE_SRC_BATCH_H

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace tombstone {

/**
 * Bounded, drop-oldest buffer of pre-serialized telemetry items (one JSON
 * object per item) for event/metric batching (spec section 16). Items are
 * accumulated by capture threads and drained by the worker thread into a
 * `{sentAtIso, items}` envelope when a flush trigger fires:
 *
 *   - count: at least flush_count items buffered;
 *   - age:   the oldest buffered item is at least flush_age old;
 *   - force: quit / pre-crash flush (drain whatever is buffered).
 *
 * Fixed capacity (max_items) with drop-oldest keeps an offline client from
 * growing unbounded (spec section 15 zero-impact budget). Thread-safe: one
 * internal mutex serializes add() and drain_if_ready().
 */
class Batch {
public:
    static constexpr std::size_t max_items = 256;
    static constexpr std::size_t flush_count = 50;
    static constexpr std::size_t max_batch_bytes = 512 * 1024;
    static constexpr std::chrono::seconds flush_age{10};

    /** Append one serialized item, dropping the oldest when at capacity.
     *  Returns true when the count trigger is now satisfied (the caller should
     *  wake the worker so it drains promptly). */
    bool add(std::string item);

    std::size_t size() const;
    bool has_items() const;

    /** Drain one bounded envelope, retaining any remaining items for the next drain. */
    std::optional<std::string> drain_if_ready(const std::string &sent_at_iso,
                                              std::chrono::steady_clock::time_point now,
                                              bool force);

    /** Drain the ready snapshot under one lock so concurrent producers cannot extend a flush. */
    std::vector<std::string> drain_envelopes_if_ready(const std::string &sent_at_iso,
                                                    std::chrono::steady_clock::time_point now,
                                                    bool force);

private:
    bool ready(std::chrono::steady_clock::time_point now, bool force) const;
    std::string drain_locked(const std::string &sent_at_iso);
    mutable std::mutex mutex_;
    std::deque<std::string> items_;
    std::chrono::steady_clock::time_point first_add_{};
};

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_BATCH_H
