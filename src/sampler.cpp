#include "sampler.h"

#include <algorithm>

namespace tombstone {

void Sampler::set_rate(std::string_view name, double rate) {
    if (name.empty()) {
        return;
    }
    const double clamped = std::clamp(rate, 0.0, 1.0);
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = rates_.find(std::string{name});
    if (existing != rates_.end()) {
        existing->second = clamped;  // update in place: an existing name is always honored
        return;
    }
    if (rates_.size() >= max_entries) {
        return;  // bounded: a new name is dropped rather than growing the map past the cap
    }
    rates_.emplace(std::string{name}, clamped);
}

bool Sampler::should_keep(std::string_view name) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = rates_.find(std::string{name});
    if (it == rates_.end()) {
        return true;  // no configured rate -> always keep
    }
    const double rate = it->second;
    // Fast paths avoid the RNG (and make the boundary rates deterministic).
    if (rate >= 1.0) {
        return true;
    }
    if (rate <= 0.0) {
        return false;
    }
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return keep(rate, dist(engine_));
}

}  // namespace tombstone
