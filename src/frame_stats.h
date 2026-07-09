#ifndef TOMBSTONE_SRC_FRAME_STATS_H
#define TOMBSTONE_SRC_FRAME_STATS_H

#include <atomic>

namespace tombstone {

/** One drained frame-stats window, ready for the heartbeat splice. All values
 *  are pre-clamped to the server contract; `valid` is false when no frame was
 *  sampled this interval (the heartbeat then omits the fields entirely). */
struct FrameStatsSnapshot {
    bool valid{false};
    double fps_avg{0.0};         // <= 1000
    double slow_frame_pct{0.0};  // <= 100
    long long hitch_count{0};    // <= 1,000,000
    long long worst_frame_ms{0}; // <= 10,000,000 (a JSON integer on the wire)
};

/**
 * Per-heartbeat-interval frame statistics accumulator (mirrors the Unity SDK's
 * TombstackFrameStats): fed once per frame via tombstone_report_frame, drained
 * by the heartbeat builder. Allocation-free on the sample path; a brief
 * spinlock (atomic_flag) guards the plain counters — the only contention is
 * one heartbeat drain per interval, so the hot path never blocks measurably.
 * Thresholds match the Unity SDK: a slow frame misses the 30 FPS budget
 * (> 33.4 ms); a hitch is visible on any target (> 250 ms).
 */
class FrameAccumulator {
public:
    static constexpr double slow_frame_ms = 33.4;
    static constexpr double hitch_frame_ms = 250.0;
    // Server contract clamps (heartbeat-schema.ts): fpsAvg <= 1000,
    // slowFramePct <= 100, hitchCount <= 1e6, worstFrameMs <= 1e7.
    static constexpr double max_fps = 1000.0;
    static constexpr long long max_hitch_count = 1000000;
    static constexpr long long max_worst_frame_ms = 10000000;

    /** Accumulate one frame's duration in milliseconds. Non-finite and <= 0
     *  samples are ignored (the first frame after a load/pause can report 0 —
     *  skip it rather than divide by it). Never throws, never allocates. */
    void sample(double frame_ms) noexcept;

    /** Drain the window into a snapshot and reset for the next interval
     *  (every drain resets, sampled or not). `valid` is false when nothing
     *  was sampled. Never throws, never allocates. */
    FrameStatsSnapshot consume() noexcept;

private:
    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    long long frame_count_{0};
    double elapsed_ms_{0.0};
    long long slow_frames_{0};
    long long hitch_frames_{0};
    double worst_frame_ms_{0.0};
};

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_FRAME_STATS_H
