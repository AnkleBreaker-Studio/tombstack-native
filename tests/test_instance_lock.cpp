#include "instance_lock.h"
#include "test_framework.h"

using tombstone::InstanceLock;

TEST_CASE("instance_lock", "only one holder at a time, and the lock frees on release") {
    ttest::TempDir dir;
    InstanceLock first;
    InstanceLock second;
    CHECK(first.try_acquire(dir.path()));
    CHECK(first.held());
    CHECK(!second.try_acquire(dir.path()));  // another process of the game: not the owner
    CHECK(!second.held());
    first.release();
    CHECK(second.try_acquire(dir.path()));  // the next one to start takes over
}

TEST_CASE("instance_lock", "an unusable directory is simply not owned") {
    ttest::TempDir dir;
    InstanceLock lock;
    CHECK(!lock.try_acquire(dir.path() / "missing" / "nested"));
    CHECK(!lock.held());
}
