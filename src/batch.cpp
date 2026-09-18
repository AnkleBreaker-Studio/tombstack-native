#include "batch.h"

#include "payloads.h"

#include <utility>
#include <vector>

namespace tombstone {

bool Batch::add(std::string item) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (items_.empty()) {
        first_add_ = std::chrono::steady_clock::now();  // age clock starts at the first item
    }
    if (items_.size() >= max_items) {
        items_.pop_front();  // bounded: drop the oldest item, never grow past the cap
    }
    items_.push_back(std::move(item));
    return items_.size() >= flush_count;
}

std::size_t Batch::size() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
}

bool Batch::has_items() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return !items_.empty();
}

std::optional<std::string> Batch::drain_if_ready(const std::string &sent_at_iso,
                                                 std::chrono::steady_clock::time_point now,
                                                 bool force) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ready(now, force)) {
        return std::nullopt;
    }
    return drain_locked(sent_at_iso);
}

bool Batch::ready(std::chrono::steady_clock::time_point now, bool force) const {
    return !items_.empty() &&
        (force || items_.size() >= flush_count || now - first_add_ >= flush_age);
}

std::vector<std::string> Batch::drain_envelopes_if_ready(const std::string &sent_at_iso,
                                                      std::chrono::steady_clock::time_point now,
                                                      bool force) {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> envelopes;
    if (!ready(now, force)) return envelopes;
    while (!items_.empty()) envelopes.push_back(drain_locked(sent_at_iso));
    return envelopes;
}

std::string Batch::drain_locked(const std::string &sent_at_iso) {
    std::vector<std::string> drained;
    drained.reserve(items_.size());
    auto bytes = build_batch_envelope(sent_at_iso, {}).size();
    while (!items_.empty()) {
        const auto next_bytes = items_.front().size() + (drained.empty() ? 0 : 1);
        if (!drained.empty() && bytes + next_bytes > max_batch_bytes) break;
        // Isolate an invalid oversized item so its rejection cannot discard valid neighbours.
        bytes += next_bytes;
        drained.push_back(std::move(items_.front()));
        items_.pop_front();
        if (bytes >= max_batch_bytes) break;
    }
    return build_batch_envelope(sent_at_iso, drained);
}

}  // namespace tombstone
