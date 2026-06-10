#include "breadcrumb_ring.h"
#include "test_framework.h"

using tombstone::Breadcrumb;
using tombstone::BreadcrumbRing;

TEST_CASE("breadcrumb_ring", "empty ring snapshots to nothing") {
    const BreadcrumbRing ring;
    CHECK(ring.snapshot().empty());
}

TEST_CASE("breadcrumb_ring", "keeps insertion order before wrapping") {
    BreadcrumbRing ring{4};
    ring.add("Info", "first", "t1");
    ring.add("Warning", "second", "t2");
    ring.add("Error", "third", "t3");
    const auto crumbs = ring.snapshot();
    CHECK_EQ(crumbs.size(), std::size_t{3});
    CHECK_EQ(crumbs[0].message, std::string{"first"});
    CHECK_EQ(crumbs[0].level, std::string{"Info"});
    CHECK_EQ(crumbs[0].ts_iso, std::string{"t1"});
    CHECK_EQ(crumbs[1].message, std::string{"second"});
    CHECK_EQ(crumbs[2].message, std::string{"third"});
}

TEST_CASE("breadcrumb_ring", "overwrites the oldest entries when full") {
    BreadcrumbRing ring{3};
    for (int i = 1; i <= 5; ++i) {
        ring.add("Info", "msg" + std::to_string(i), "t" + std::to_string(i));
    }
    const auto crumbs = ring.snapshot();
    CHECK_EQ(crumbs.size(), std::size_t{3});
    CHECK_EQ(crumbs[0].message, std::string{"msg3"});  // oldest surviving
    CHECK_EQ(crumbs[1].message, std::string{"msg4"});
    CHECK_EQ(crumbs[2].message, std::string{"msg5"});  // newest
}

TEST_CASE("breadcrumb_ring", "clamps message and level to the wire limits") {
    BreadcrumbRing ring;
    ring.add(std::string(40, 'L'), std::string(600, 'm'), "t");
    const auto crumbs = ring.snapshot();
    CHECK_EQ(crumbs.size(), std::size_t{1});
    CHECK_EQ(crumbs[0].message.size(), BreadcrumbRing::max_message_bytes);
    CHECK_EQ(crumbs[0].level.size(), BreadcrumbRing::max_level_bytes);
}

TEST_CASE("breadcrumb_ring", "ignores empty messages and supports clear") {
    BreadcrumbRing ring;
    ring.add("Info", "", "t");
    CHECK(ring.snapshot().empty());
    ring.add("Info", "kept", "t");
    CHECK_EQ(ring.snapshot().size(), std::size_t{1});
    ring.clear();
    CHECK(ring.snapshot().empty());
}

TEST_CASE("breadcrumb_ring", "clear after wrap fully resets and ring stays reusable") {
    // Mirrors the consent-revoke purge (set_consent(false) -> breadcrumbs_.clear()):
    // after a wrap, clear must drop everything and leave the ring usable from scratch.
    BreadcrumbRing ring{3};
    for (int i = 1; i <= 5; ++i) {
        ring.add("Info", "msg" + std::to_string(i), "t" + std::to_string(i));
    }
    ring.clear();
    CHECK(ring.snapshot().empty());

    ring.add("Warning", "after-clear-1", "t6");
    ring.add("Error", "after-clear-2", "t7");
    const auto crumbs = ring.snapshot();
    CHECK_EQ(crumbs.size(), std::size_t{2});
    CHECK_EQ(crumbs[0].message, std::string{"after-clear-1"});  // oldest first, no stale entries
    CHECK_EQ(crumbs[1].message, std::string{"after-clear-2"});
}

TEST_CASE("breadcrumb_ring", "default capacity is 64") {
    BreadcrumbRing ring;  // default capacity
    for (int i = 0; i < 100; ++i) {
        ring.add("Info", "m" + std::to_string(i), "t");
    }
    CHECK_EQ(ring.snapshot().size(), BreadcrumbRing::default_capacity);
}
