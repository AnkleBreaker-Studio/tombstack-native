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
    if (items_.empty()) {
        return std::nullopt;
    }
    const bool by_count = items_.size() >= flush_count;
    const bool by_age = (now - first_add_) >= flush_age;
    if (!force && !by_count && !by_age) {
        return std::nullopt;
    }
    std::vector<std::string> drained;
    drained.reserve(items_.size());
    for (std::string &item : items_) {
        drained.push_back(std::move(item));
    }
    items_.clear();
    return build_batch_envelope(sent_at_iso, drained);
}

}  // namespace tombstone
