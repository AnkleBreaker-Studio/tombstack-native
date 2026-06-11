#ifndef TOMBSTONE_SRC_SAMPLER_H
#define TOMBSTONE_SRC_SAMPLER_H

#include <cstddef>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tombstone {

/**
 * Per-name client-side sampler (K1). A named event/metric stream can be assigned
 * a keep-rate in [0,1] via set_rate(); should_keep() then probabilistically drops
 * samples below that rate BEFORE they are buffered, trimming volume for chatty
 * streams. A name with no configured rate is always kept (rate defaults to 1).
 *
 * The rate map is bounded (drop-on-full) so a hostile or buggy caller cannot grow
 * it without limit, and every access is mutex-guarded (rates are set from the game
 * thread and read from capture threads). Fail-soft: nothing here ever throws.
 */
class Sampler {
public:
    static constexpr std::size_t max_entries = 256;

    /** Set the keep-rate for `name` (clamped to [0,1]). A no-op for an empty name
     *  or once the bounded map is full and `name` is not already present. */
    void set_rate(std::string_view name, double rate);

    /** True when a sample for `name` should be kept. Names with no configured rate
     *  are always kept; otherwise a uniform roll in [0,1) is compared to the rate. */
    bool should_keep(std::string_view name);

    /** Pure keep decision: keep when `roll` (expected in [0,1)) is below `rate`.
     *  rate >= 1 always keeps; rate <= 0 always drops. Exposed for testing. */
    static bool keep(double rate, double roll) noexcept { return roll < rate; }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, double> rates_;
    std::mt19937_64 engine_{std::random_device{}()};
};

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_SAMPLER_H
