#include "frame_stats.h"

#include <cmath>

namespace tombstone {

namespace {

/** RAII spin guard over the accumulator's atomic_flag. The critical sections
 *  are a handful of arithmetic ops, so spinning (never sleeping) is cheaper
 *  than a mutex on the every-frame hot path. */
class SpinGuard {
public:
    explicit SpinGuard(std::atomic_flag &flag) noexcept : flag_(flag) {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            // spin: the holder is mid-arithmetic, a few cycles away
        }
    }
    ~SpinGuard() { flag_.clear(std::memory_order_release); }

    SpinGuard(const SpinGuard &) = delete;
    SpinGuard &operator=(const SpinGuard &) = delete;

private:
    std::atomic_flag &flag_;
};

}  // namespace

void FrameAccumulator::sample(double frame_ms) noexcept {
    if (!std::isfinite(frame_ms) || frame_ms <= 0.0) {
        return;  // the first frame after a load/pause can report 0 — skip it
    }
    const SpinGuard guard{lock_};
    ++frame_count_;
    elapsed_ms_ += frame_ms;
    if (frame_ms > slow_frame_ms) {
        ++slow_frames_;
    }
    if (frame_ms > hitch_frame_ms && hitch_frames_ < max_hitch_count) {
        ++hitch_frames_;
    }
    if (frame_ms > worst_frame_ms_) {
        worst_frame_ms_ = frame_ms;
    }
}

FrameStatsSnapshot FrameAccumulator::consume() noexcept {
    const SpinGuard guard{lock_};
    FrameStatsSnapshot out;
    if (frame_count_ > 0 && elapsed_ms_ > 0.0) {
        out.valid = true;
        double fps = static_cast<double>(frame_count_) * 1000.0 / elapsed_ms_;
        if (fps > max_fps) {
            fps = max_fps;
        }
        out.fps_avg = fps;
        double slow_pct =
            static_cast<double>(slow_frames_) * 100.0 / static_cast<double>(frame_count_);
        if (slow_pct > 100.0) {
            slow_pct = 100.0;
        }
        out.slow_frame_pct = slow_pct;
        out.hitch_count = hitch_frames_;  // already capped at max_hitch_count in sample()
        // Clamp BEFORE the integer cast: a huge double -> long long cast is UB.
        const double worst_capped = worst_frame_ms_ > static_cast<double>(max_worst_frame_ms)
                                        ? static_cast<double>(max_worst_frame_ms)
                                        : worst_frame_ms_;
        out.worst_frame_ms = static_cast<long long>(worst_capped);
    }
    frame_count_ = 0;
    elapsed_ms_ = 0.0;
    slow_frames_ = 0;
    hitch_frames_ = 0;
    worst_frame_ms_ = 0.0;
    return out;
}

}  // namespace tombstone
