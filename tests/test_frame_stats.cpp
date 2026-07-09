#include "frame_stats.h"
#include "test_framework.h"

#include <limits>

using tombstone::FrameAccumulator;
using tombstone::FrameStatsSnapshot;

TEST_CASE("frame_stats", "an empty window drains as invalid") {
    FrameAccumulator acc;
    const FrameStatsSnapshot snapshot = acc.consume();
    CHECK(!snapshot.valid);
}

TEST_CASE("frame_stats", "fps average is frames over elapsed time") {
    FrameAccumulator acc;
    // 3 frames of 20 ms = 60 ms elapsed -> 3 / 0.06 s = 50 fps exactly.
    acc.sample(20.0);
    acc.sample(20.0);
    acc.sample(20.0);
    const FrameStatsSnapshot snapshot = acc.consume();
    CHECK(snapshot.valid);
    CHECK_EQ(snapshot.fps_avg, 50.0);
    CHECK_EQ(snapshot.slow_frame_pct, 0.0);
    CHECK_EQ(snapshot.hitch_count, 0LL);
    CHECK_EQ(snapshot.worst_frame_ms, 20LL);
}

TEST_CASE("frame_stats", "slow frames are counted above the 33.4ms threshold") {
    FrameAccumulator acc;
    acc.sample(33.4);  // exactly at the threshold: NOT slow (strictly greater)
    acc.sample(40.0);  // slow
    const FrameStatsSnapshot snapshot = acc.consume();
    CHECK(snapshot.valid);
    CHECK_EQ(snapshot.slow_frame_pct, 50.0);  // 1 of 2 frames
    CHECK_EQ(snapshot.hitch_count, 0LL);      // neither exceeds the hitch bar
}

TEST_CASE("frame_stats", "hitches are counted above the 250ms threshold") {
    FrameAccumulator acc;
    acc.sample(250.0);  // exactly at the threshold: NOT a hitch
    acc.sample(251.0);  // a hitch (and slow, and the worst frame)
    acc.sample(10.0);
    const FrameStatsSnapshot snapshot = acc.consume();
    CHECK(snapshot.valid);
    CHECK_EQ(snapshot.hitch_count, 1LL);
    CHECK_EQ(snapshot.worst_frame_ms, 251LL);
}

TEST_CASE("frame_stats", "consume resets the window") {
    FrameAccumulator acc;
    acc.sample(300.0);
    const FrameStatsSnapshot first = acc.consume();
    CHECK(first.valid);
    CHECK_EQ(first.hitch_count, 1LL);
    // The window reset: a second drain with no new samples is invalid, and a
    // fresh sample starts counting from zero.
    const FrameStatsSnapshot empty = acc.consume();
    CHECK(!empty.valid);
    acc.sample(10.0);
    const FrameStatsSnapshot second = acc.consume();
    CHECK(second.valid);
    CHECK_EQ(second.hitch_count, 0LL);
    CHECK_EQ(second.worst_frame_ms, 10LL);
}

TEST_CASE("frame_stats", "non-finite and non-positive samples are ignored") {
    FrameAccumulator acc;
    acc.sample(0.0);
    acc.sample(-5.0);
    acc.sample(std::numeric_limits<double>::quiet_NaN());
    acc.sample(std::numeric_limits<double>::infinity());
    CHECK(!acc.consume().valid);  // nothing usable was sampled
    // A valid sample after garbage still counts normally.
    acc.sample(std::numeric_limits<double>::quiet_NaN());
    acc.sample(16.0);
    const FrameStatsSnapshot snapshot = acc.consume();
    CHECK(snapshot.valid);
    CHECK_EQ(snapshot.fps_avg, 62.5);  // 1 frame / 16 ms
}

TEST_CASE("frame_stats", "fps average clamps to the schema maximum") {
    FrameAccumulator acc;
    acc.sample(0.5);  // 2000 fps -> clamped to 1000 (heartbeat-schema fpsAvg max)
    const FrameStatsSnapshot snapshot = acc.consume();
    CHECK(snapshot.valid);
    CHECK_EQ(snapshot.fps_avg, 1000.0);
}

TEST_CASE("frame_stats", "worst frame clamps to the schema maximum") {
    FrameAccumulator acc;
    acc.sample(20000000.0);  // a 20,000 s "frame" -> clamped to 10,000,000 ms
    const FrameStatsSnapshot snapshot = acc.consume();
    CHECK(snapshot.valid);
    CHECK_EQ(snapshot.worst_frame_ms, FrameAccumulator::max_worst_frame_ms);
    CHECK_EQ(snapshot.hitch_count, 1LL);  // it is also (very much) a hitch
}

TEST_CASE("frame_stats", "hitch count saturates at the schema maximum") {
    FrameAccumulator acc;
    for (long long i = 0; i < FrameAccumulator::max_hitch_count + 100; ++i) {
        acc.sample(300.0);
    }
    const FrameStatsSnapshot snapshot = acc.consume();
    CHECK(snapshot.valid);
    CHECK_EQ(snapshot.hitch_count, FrameAccumulator::max_hitch_count);
}
