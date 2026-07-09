// StartGate latch semantics — pins the end-to-end auto_start_session flow at
// the unit level. This gate is the EXACT object Client consults on the
// heartbeat-send and batch-drain paths (client.cpp: heartbeat_loop,
// drain_ready_batches, flush_all_batches) and opens in start_session(), so
// these tests prove the release semantics without a network transport:
//   arm(true)   == options.auto_start_session = 1 (default)
//   arm(false)  == options.auto_start_session = 0
//   set()       == tombstone_start_session() (post-init, or replayed pre-init)
//   is_set()    == "may heartbeats leave / may batches drain?"

#include "start_gate.h"
#include "test_framework.h"

using tombstone::StartGate;

TEST_CASE("start_gate", "default constructed gate is open") {
    StartGate gate;
    CHECK(gate.is_set());
}

TEST_CASE("start_gate", "arm true opens immediately (auto_start_session default)") {
    StartGate gate;
    gate.arm(true);
    CHECK(gate.is_set());
}

TEST_CASE("start_gate", "arm false holds the gate closed (auto_start_session 0)") {
    StartGate gate;
    gate.arm(false);
    CHECK(!gate.is_set());
}

TEST_CASE("start_gate", "post-init start_session releases the closed gate exactly once") {
    // auto_start_session = 0, then tombstone_start_session() after init: the
    // first set() reports the closed->open transition (heartbeat kick + worker
    // wake happen on that edge in Client::start_session).
    StartGate gate;
    gate.arm(false);
    CHECK(!gate.is_set());
    CHECK(gate.set());
    CHECK(gate.is_set());
}

TEST_CASE("start_gate", "pre-init start_session is remembered through arm") {
    // tombstone_start_session() BEFORE init (PreInitStore remembers the
    // request): the explicit request survives arming with auto_start_session=0
    // — the gate must come out open, never re-closed by the init option.
    StartGate gate;
    (void)gate.set();  // pre-init request (gate was default-open: no transition)
    gate.arm(false);
    CHECK(gate.is_set());
}

TEST_CASE("start_gate", "set is one-way: open stays open") {
    StartGate gate;
    gate.arm(false);
    CHECK(gate.set());
    CHECK(gate.is_set());
    CHECK(!gate.set());   // no second first-transition
    CHECK(gate.is_set()); // and the gate never closes again
}

TEST_CASE("start_gate", "set is idempotent: only the first call reports the transition") {
    StartGate gate;
    gate.arm(false);
    int first_transitions = 0;
    for (int i = 0; i < 5; ++i) {
        if (gate.set()) {
            ++first_transitions;
        }
    }
    CHECK_EQ(first_transitions, 1);
    CHECK(gate.is_set());
}

TEST_CASE("start_gate", "set on an auto-started gate is a no-op transition") {
    // auto_start_session = 1 then a redundant tombstone_start_session(): the
    // gate was never closed, so no kick edge fires (matches the pre-refactor
    // exchange(true) on an already-true atomic).
    StartGate gate;
    gate.arm(true);
    CHECK(!gate.set());
    CHECK(gate.is_set());
}
