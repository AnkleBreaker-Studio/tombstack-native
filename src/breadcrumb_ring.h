#ifndef TOMBSTONE_SRC_BREADCRUMB_RING_H
#define TOMBSTONE_SRC_BREADCRUMB_RING_H

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace tombstone {

/** One wire breadcrumb: { tsIso, level, message }. */
struct Breadcrumb {
    std::string ts_iso;
    std::string level;
    std::string message;
};

/**
 * Fixed-capacity ring of the most recent breadcrumbs, attached to crash and
 * bug-report payloads. Slots are preallocated once; recording overwrites the
 * oldest slot in place. Thread-safe (breadcrumbs arrive from any thread).
 */
class BreadcrumbRing {
public:
    static constexpr std::size_t default_capacity = 64;
    static constexpr std::size_t max_message_bytes = 512;
    static constexpr std::size_t max_level_bytes = 16;

    explicit BreadcrumbRing(std::size_t capacity = default_capacity);

    /** Record one breadcrumb (clamped to the wire limits). Empty messages are ignored. */
    void add(std::string_view level, std::string_view message, std::string ts_iso);

    /** Copy of the buffered crumbs, oldest first. Empty when nothing recorded. */
    std::vector<Breadcrumb> snapshot() const;

    /** Drop everything (consent revoked). */
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<Breadcrumb> slots_;
    std::size_t head_{0};
    std::size_t count_{0};
};

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_BREADCRUMB_RING_H
