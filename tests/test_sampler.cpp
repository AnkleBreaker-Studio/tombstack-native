#include "sampler.h"
#include "test_framework.h"

#include <string>

using tombstone::Sampler;

TEST_CASE("sampler", "keeps everything for an unconfigured name") {
    Sampler sampler;
    for (int i = 0; i < 100; ++i) {
        CHECK(sampler.should_keep("never-set"));
    }
}

TEST_CASE("sampler", "rate 0 always drops, rate 1 always keeps") {
    Sampler sampler;
    sampler.set_rate("dropped", 0.0);
    sampler.set_rate("kept", 1.0);
    for (int i = 0; i < 100; ++i) {
        CHECK(!sampler.should_keep("dropped"));
        CHECK(sampler.should_keep("kept"));
    }
}

TEST_CASE("sampler", "rate clamps out-of-range values") {
    Sampler sampler;
    sampler.set_rate("over", 5.0);    // clamps to 1.0 -> always keep
    sampler.set_rate("under", -3.0);  // clamps to 0.0 -> always drop
    for (int i = 0; i < 50; ++i) {
        CHECK(sampler.should_keep("over"));
        CHECK(!sampler.should_keep("under"));
    }
}

TEST_CASE("sampler", "a later set_rate replaces the earlier one") {
    Sampler sampler;
    sampler.set_rate("flip", 0.0);
    CHECK(!sampler.should_keep("flip"));
    sampler.set_rate("flip", 1.0);
    CHECK(sampler.should_keep("flip"));
}

TEST_CASE("sampler", "pure keep decision compares the roll to the rate") {
    // roll < rate keeps; the boundary rates are exact.
    CHECK(Sampler::keep(1.0, 0.0));
    CHECK(Sampler::keep(1.0, 0.999999));
    CHECK(!Sampler::keep(0.0, 0.0));
    CHECK(Sampler::keep(0.5, 0.25));
    CHECK(!Sampler::keep(0.5, 0.75));
    CHECK(!Sampler::keep(0.5, 0.5));  // roll == rate is a drop (half-open interval)
}

TEST_CASE("sampler", "an empty name is ignored and still keeps") {
    Sampler sampler;
    sampler.set_rate("", 0.0);       // no-op: empty name is not stored
    CHECK(sampler.should_keep(""));  // unconfigured -> kept
}

TEST_CASE("sampler", "is bounded: a new name past the cap is not stored") {
    Sampler sampler;
    // Fill the table to capacity with drop-everything rates.
    for (std::size_t i = 0; i < Sampler::max_entries; ++i) {
        sampler.set_rate("n" + std::to_string(i), 0.0);
    }
    CHECK(!sampler.should_keep("n0"));  // a stored name still drops
    // A brand-new name past the cap cannot be inserted -> defaults to keep.
    sampler.set_rate("overflow", 0.0);
    CHECK(sampler.should_keep("overflow"));
}
