#ifndef TOMBSTONE_SRC_START_GATE_H
#define TOMBSTONE_SRC_START_GATE_H

#include <atomic>

namespace tombstone {

/**
 * The v0.7 send gate (Unity 0.15 parity) as a standalone, testable unit: a
 * one-way idempotent latch that holds heartbeats and event/metric batch
 * drains until the session is explicitly started.
 *
 * Lifecycle (exactly the Client + pre-init flow):
 *  - Default-constructed OPEN — matches the historical
 *    `collecting_started_{true}` member default, so a client that is never
 *    armed behaves like `auto_start_session = 1`.
 *  - arm(auto_start) seeds the gate once, inside Client::init, from
 *    options.auto_start_session. A set() that happened BEFORE arming (the
 *    pre-init tombstone_start_session request, remembered by PreInitStore
 *    and replayed into the client) survives arm(false): the explicit
 *    request always wins over the init option.
 *  - set() opens the gate forever (one-way, idempotent) and returns true
 *    ONLY on the first closed->open transition, so the caller can kick the
 *    heartbeat loop / wake the worker exactly once.
 *  - is_set() is the query on the heartbeat-send and batch-drain paths.
 *
 * Threading: set() / is_set() are safe from any thread (plain atomics).
 * arm() must not race set()/is_set(): it runs once inside Client::init,
 * before the client is published to other threads (the C ABI layer holds
 * g_client_mutex across init + pre-init replay), exactly like the plain
 * assignment it replaced.
 */
class StartGate {
public:
    StartGate() noexcept = default;

    /** Seed the gate from options.auto_start_session at init. An earlier
     *  explicit set() is remembered and survives arm(false). */
    void arm(bool auto_start) noexcept { open_.store(auto_start || requested_.load()); }

    /** One-way idempotent open. Returns true only on the FIRST closed->open
     *  transition (the caller kicks heartbeat + worker on that edge). */
    bool set() noexcept {
        requested_.store(true);
        return !open_.exchange(true);
    }

    /** True once the gate is open (sends allowed). Relaxed load: the gate is
     *  a monotonic flag and every consumer tolerates one stale read — same
     *  ordering the inline atomic used before the extraction. */
    bool is_set() const noexcept { return open_.load(std::memory_order_relaxed); }

private:
    // Open by default: matches the pre-refactor collecting_started_{true}.
    std::atomic<bool> open_{true};
    // Remembers an explicit set() so it survives a later arm(false).
    std::atomic<bool> requested_{false};
};

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_START_GATE_H
