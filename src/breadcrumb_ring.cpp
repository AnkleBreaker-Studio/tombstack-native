#include "breadcrumb_ring.h"

#include "json_writer.h"

#include <utility>

namespace tombstone {

BreadcrumbRing::BreadcrumbRing(std::size_t capacity) {
    slots_.resize(capacity > 0 ? capacity : default_capacity);
}

void BreadcrumbRing::add(std::string_view level, std::string_view message, std::string ts_iso) {
    if (message.empty()) {
        return;
    }
    const std::string_view clamped_level = utf8_safe_truncate(level, max_level_bytes);
    const std::string_view clamped_message = utf8_safe_truncate(message, max_message_bytes);

    const std::lock_guard<std::mutex> lock(mutex_);
    Breadcrumb &slot = slots_[head_];
    slot.ts_iso = std::move(ts_iso);
    slot.level.assign(clamped_level);
    slot.message.assign(clamped_message);
    head_ = (head_ + 1) % slots_.size();
    if (count_ < slots_.size()) {
        ++count_;
    }
}

std::vector<Breadcrumb> BreadcrumbRing::snapshot() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Breadcrumb> out;
    out.reserve(count_);
    const std::size_t capacity = slots_.size();
    const std::size_t start = (head_ + capacity - count_) % capacity;
    for (std::size_t i = 0; i < count_; ++i) {
        out.push_back(slots_[(start + i) % capacity]);
    }
    return out;
}

void BreadcrumbRing::clear() {
    const std::lock_guard<std::mutex> lock(mutex_);
    head_ = 0;
    count_ = 0;
    // Scrub slot memory too: clear() is called on consent revoke / GDPR reset, so pre-revoke
    // breadcrumb text (potentially PII) must not linger in the process until later overwrites.
    for (auto &slot : slots_) {
        slot.ts_iso.clear();
        slot.level.clear();
        slot.message.clear();
    }
}

}  // namespace tombstone
