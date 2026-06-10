#include "dedupe.h"

namespace tombstone {

DedupeWindow::DedupeWindow(std::chrono::seconds window) : window_(window) {}

DedupeWindow::Decision DedupeWindow::check(const std::string &signature,
                                           std::chrono::steady_clock::time_point now) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = recent_.find(signature);
    if (found != recent_.end()) {
        Entry &entry = found->second;
        if (now - entry.last_sent < window_) {
            ++entry.suppressed;
            return Decision{false, entry.suppressed};
        }
        entry.last_sent = now;
        entry.suppressed = 0;
        return Decision{true, 0};
    }
    if (recent_.size() >= max_tracked_signatures) {
        recent_.clear();  // bounded: worst case re-allows one early report per signature
    }
    recent_.emplace(signature, Entry{now, 0});
    return Decision{true, 0};
}

}  // namespace tombstone
