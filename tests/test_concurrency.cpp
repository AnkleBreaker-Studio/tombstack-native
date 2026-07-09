// Concurrency hammer suite — many threads against the three units that see
// cross-thread traffic in production: FrameAccumulator (every-frame spinlock),
// PreInitStore (serialized by the ABI mutex), and StartGate (lock-free latch).
//
// Rules of this suite:
//  - Synchronization is atomics only (a spin "go" flag as the start barrier);
//    no sleeps-as-sync anywhere.
//  - NEVER assert inside a worker thread: a ttest::Failure escaping a
//    std::thread body calls std::terminate. Threads record violations into
//    atomics; the main thread asserts after join().
//  - Total runtime stays well under ~2 s (tens of thousands of tiny ops).

#include "frame_stats.h"
#include "preinit.h"
#include "start_gate.h"
#include "test_framework.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

/** Spin until the shared go flag flips — a start barrier with no sleeps. */
void wait_for_go(const std::atomic<bool> &go) {
    while (!go.load(std::memory_order_acquire)) {
        // spin: the main thread flips the flag as soon as all threads exist
    }
}

}  // namespace

TEST_CASE("concurrency", "frame accumulator: concurrent sample vs consume loses nothing") {
    // Every sample is a 300 ms hitch, so hitch_count is an exact tally of the
    // samples each drained window absorbed: the spinlock serializes sample()
    // and consume(), so summed across all windows nothing may be lost or
    // double-counted — and every snapshot must stay finite and in-contract.
    tombstone::FrameAccumulator acc;
    constexpr int n_producers = 4;
    constexpr int samples_per_producer = 10000;
    constexpr double hitch_ms = 300.0;  // > hitch threshold (250) and > slow (33.4)

    std::atomic<bool> go{false};
    std::atomic<int> producers_done{0};
    std::atomic<long long> consumed_hitches{0};
    std::atomic<int> violations{0};

    auto drain_and_check = [&](const tombstone::FrameStatsSnapshot &snap) {
        if (!snap.valid) {
            return;
        }
        const bool sane = std::isfinite(snap.fps_avg) && snap.fps_avg > 0.0 &&
                          snap.fps_avg <= tombstone::FrameAccumulator::max_fps &&
                          std::isfinite(snap.slow_frame_pct) && snap.slow_frame_pct >= 0.0 &&
                          snap.slow_frame_pct <= 100.0 && snap.hitch_count >= 0 &&
                          snap.worst_frame_ms >= 0 &&
                          snap.worst_frame_ms <= static_cast<long long>(hitch_ms);
        if (!sane) {
            violations.fetch_add(1, std::memory_order_relaxed);
        }
        consumed_hitches.fetch_add(snap.hitch_count, std::memory_order_relaxed);
    };

    std::vector<std::thread> producers;
    producers.reserve(n_producers);
    for (int t = 0; t < n_producers; ++t) {
        producers.emplace_back([&] {
            wait_for_go(go);
            for (int i = 0; i < samples_per_producer; ++i) {
                acc.sample(hitch_ms);
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }
    std::thread consumer([&] {
        wait_for_go(go);
        while (producers_done.load(std::memory_order_acquire) < n_producers) {
            drain_and_check(acc.consume());
        }
    });

    go.store(true, std::memory_order_release);
    for (std::thread &t : producers) {
        t.join();
    }
    consumer.join();
    drain_and_check(acc.consume());  // final window: whatever the racing drains missed

    CHECK_EQ(violations.load(), 0);
    CHECK_EQ(consumed_hitches.load(),
             static_cast<long long>(n_producers) * samples_per_producer);
}

TEST_CASE("concurrency", "preinit store: concurrent serialized adds respect the caps") {
    // PreInitStore is unsynchronized BY CONTRACT (preinit.h): the C ABI layer
    // serializes every access under g_client_mutex. This test mirrors that —
    // one mutex around each store call — because the property under load is
    // cap integrity and replay-safety, not lock-free internals (racing the
    // raw store would be UB by its own documented contract).
    tombstone::PreInitStore store;
    std::mutex store_mutex;
    constexpr int n_threads = 8;
    constexpr int per_kind = 50;  // 8*50 = 400 crumbs > 64 cap; 8*100 = 800 tracks > 128 cap

    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(n_threads);
    for (int t = 0; t < n_threads; ++t) {
        threads.emplace_back([&, t] {
            wait_for_go(go);
            for (int i = 0; i < per_kind; ++i) {
                const std::string tag = std::to_string(t) + ":" + std::to_string(i);
                {
                    const std::lock_guard<std::mutex> lock(store_mutex);
                    store.add_breadcrumb(1, "crumb " + tag);
                }
                {
                    const std::lock_guard<std::mutex> lock(store_mutex);
                    (void)store.add_event("evt." + tag, {});
                }
                {
                    const std::lock_guard<std::mutex> lock(store_mutex);
                    (void)store.add_metric("met." + tag, static_cast<double>(i), "ms");
                }
            }
            const std::lock_guard<std::mutex> lock(store_mutex);
            store.set_user("user-" + std::to_string(t), "");
        });
    }
    go.store(true, std::memory_order_release);
    for (std::thread &th : threads) {
        th.join();
    }

    // Caps held exactly: drop-oldest kept the crumb cap, drop-newest the
    // combined track cap.
    CHECK(store.breadcrumbs().size() <= tombstone::PreInitStore::max_breadcrumbs);
    CHECK(store.tracks().size() <= tombstone::PreInitStore::max_tracks);
    CHECK_EQ(store.breadcrumbs().size(), tombstone::PreInitStore::max_breadcrumbs);
    CHECK_EQ(store.tracks().size(), tombstone::PreInitStore::max_tracks);

    // Replay-style read pass (the shape replay_preinit walks): every surviving
    // item must be intact — non-empty, fully-formed strings, no torn entries.
    std::size_t replayed = 0;
    for (const auto &crumb : store.breadcrumbs()) {
        if (!crumb.message.empty()) {
            ++replayed;
        }
    }
    for (const auto &track : store.tracks()) {
        if (!track.name.empty()) {
            ++replayed;
        }
    }
    CHECK_EQ(replayed,
             tombstone::PreInitStore::max_breadcrumbs + tombstone::PreInitStore::max_tracks);
    CHECK(store.has_user());
    CHECK(!store.user_id().empty());  // last-write-wins landed on SOME thread's value
}

TEST_CASE("concurrency", "start gate: racing set vs query yields exactly one release") {
    tombstone::StartGate gate;
    gate.arm(false);
    constexpr int n_setters = 4;
    constexpr int n_queriers = 4;

    std::atomic<bool> go{false};
    std::atomic<int> first_transitions{0};
    std::atomic<int> observed_open{0};
    std::vector<std::thread> threads;
    threads.reserve(n_setters + n_queriers);
    for (int t = 0; t < n_setters; ++t) {
        threads.emplace_back([&] {
            wait_for_go(go);
            if (gate.set()) {
                first_transitions.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int t = 0; t < n_queriers; ++t) {
        threads.emplace_back([&] {
            wait_for_go(go);
            while (!gate.is_set()) {
                // spin: guaranteed to end — a setter thread will open the gate
            }
            observed_open.fetch_add(1, std::memory_order_relaxed);
        });
    }
    go.store(true, std::memory_order_release);
    for (std::thread &th : threads) {
        th.join();
    }

    CHECK_EQ(first_transitions.load(), 1);  // the kick edge fired exactly once
    CHECK_EQ(observed_open.load(), n_queriers);
    CHECK(gate.is_set());
}
