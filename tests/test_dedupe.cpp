#include "dedupe.h"
#include "test_framework.h"

using tombstone::DedupeWindow;

namespace {
using Clock = std::chrono::steady_clock;
const Clock::time_point t0 = Clock::now();
}  // namespace

TEST_CASE("dedupe", "first report of a signature is allowed") {
    DedupeWindow window;
    CHECK(window.check("sig-a", t0).allowed);
}

TEST_CASE("dedupe", "repeats inside the window are suppressed with a count") {
    DedupeWindow window;
    CHECK(window.check("sig-a", t0).allowed);
    const auto first_repeat = window.check("sig-a", t0 + std::chrono::seconds{1});
    CHECK(!first_repeat.allowed);
    CHECK_EQ(first_repeat.suppressed_count, 1);
    const auto second_repeat = window.check("sig-a", t0 + std::chrono::seconds{30});
    CHECK(!second_repeat.allowed);
    CHECK_EQ(second_repeat.suppressed_count, 2);
}

TEST_CASE("dedupe", "the same signature reports again after the window") {
    DedupeWindow window;
    CHECK(window.check("sig-a", t0).allowed);
    CHECK(!window.check("sig-a", t0 + std::chrono::seconds{59}).allowed);
    CHECK(window.check("sig-a", t0 + std::chrono::seconds{61}).allowed);
    // ...and the suppression count restarts.
    const auto repeat = window.check("sig-a", t0 + std::chrono::seconds{62});
    CHECK(!repeat.allowed);
    CHECK_EQ(repeat.suppressed_count, 1);
}

TEST_CASE("dedupe", "different signatures do not interfere") {
    DedupeWindow window;
    CHECK(window.check("sig-a", t0).allowed);
    CHECK(window.check("sig-b", t0).allowed);
    CHECK(!window.check("sig-a", t0 + std::chrono::seconds{1}).allowed);
    CHECK(!window.check("sig-b", t0 + std::chrono::seconds{1}).allowed);
}

TEST_CASE("dedupe", "bounded map never drops a brand new crash") {
    DedupeWindow window;
    for (std::size_t i = 0; i < DedupeWindow::max_tracked_signatures; ++i) {
        CHECK(window.check("sig-" + std::to_string(i), t0).allowed);
    }
    // At capacity the map resets — the new signature still reports.
    CHECK(window.check("sig-overflow", t0 + std::chrono::seconds{1}).allowed);
}

TEST_CASE("dedupe", "custom window length is honored") {
    DedupeWindow window{std::chrono::seconds{5}};
    CHECK(window.check("sig", t0).allowed);
    CHECK(!window.check("sig", t0 + std::chrono::seconds{4}).allowed);
    CHECK(window.check("sig", t0 + std::chrono::seconds{6}).allowed);
}
