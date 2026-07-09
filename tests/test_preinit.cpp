#include "preinit.h"
#include "test_framework.h"

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using tombstone::PreInitStore;

namespace {

std::vector<std::pair<std::string, std::string>> attrs(
    std::initializer_list<std::pair<std::string, std::string>> init) {
    return std::vector<std::pair<std::string, std::string>>{init};
}

}  // namespace

TEST_CASE("preinit", "starts empty with nothing remembered") {
    PreInitStore store;
    CHECK(store.breadcrumbs().empty());
    CHECK(store.tracks().empty());
    CHECK(!store.has_user());
    CHECK(!store.has_environment());
    CHECK(!store.has_user_metadata());
    CHECK(!store.has_consent());
    CHECK(!store.start_session_requested());
}

TEST_CASE("preinit", "breadcrumbs drop the oldest past 64") {
    PreInitStore store;
    for (int i = 0; i < 70; ++i) {
        store.add_breadcrumb(1, "crumb-" + std::to_string(i));
    }
    CHECK_EQ(store.breadcrumbs().size(), PreInitStore::max_breadcrumbs);
    // The 6 oldest (0..5) were dropped; the newest survives at the back.
    CHECK_EQ(store.breadcrumbs().front().message, std::string{"crumb-6"});
    CHECK_EQ(store.breadcrumbs().back().message, std::string{"crumb-69"});
}

TEST_CASE("preinit", "events and metrics share a 128 drop-newest cap") {
    PreInitStore store;
    // Fill with 100 events + 28 metrics = exactly at the cap...
    for (int i = 0; i < 100; ++i) {
        CHECK(store.add_event("evt-" + std::to_string(i), {}));
    }
    for (int i = 0; i < 28; ++i) {
        CHECK(store.add_metric("met-" + std::to_string(i), 1.0, "ms"));
    }
    CHECK_EQ(store.tracks().size(), PreInitStore::max_tracks);
    // ...then everything else is dropped (drop-NEWEST: the buffer is full).
    CHECK(!store.add_event("late-event", {}));
    CHECK(!store.add_metric("late-metric", 2.0, ""));
    CHECK_EQ(store.tracks().size(), PreInitStore::max_tracks);
    CHECK_EQ(store.tracks().front().name, std::string{"evt-0"});
    CHECK_EQ(store.tracks().back().name, std::string{"met-27"});
}

TEST_CASE("preinit", "tracks preserve the combined insertion order") {
    PreInitStore store;
    CHECK(store.add_event("first-event", attrs({{"k", "v"}})));
    CHECK(store.add_metric("then-metric", 42.0, "hz"));
    CHECK(store.add_event("last-event", {}));
    CHECK_EQ(store.tracks().size(), std::size_t{3});
    CHECK(!store.tracks()[0].is_metric);
    CHECK_EQ(store.tracks()[0].name, std::string{"first-event"});
    CHECK_EQ(store.tracks()[0].attributes.size(), std::size_t{1});
    CHECK(store.tracks()[1].is_metric);
    CHECK_EQ(store.tracks()[1].value, 42.0);
    CHECK_EQ(store.tracks()[1].unit, std::string{"hz"});
    CHECK(!store.tracks()[2].is_metric);
    CHECK_EQ(store.tracks()[2].name, std::string{"last-event"});
}

TEST_CASE("preinit", "identity, environment, metadata, and consent are last-write-wins") {
    PreInitStore store;
    store.set_user("player-1", "steam-1");
    store.set_user("player-2", "");
    CHECK(store.has_user());
    CHECK_EQ(store.user_id(), std::string{"player-2"});
    CHECK_EQ(store.steam_id(), std::string{""});

    store.set_environment("staging");
    store.set_environment("dev");
    CHECK(store.has_environment());
    CHECK_EQ(store.environment(), std::string{"dev"});

    store.set_user_metadata(attrs({{"displayName", "Alpha"}}));
    store.set_user_metadata(attrs({{"displayName", "Beta"}, {"clan", "Reapers"}}));
    CHECK(store.has_user_metadata());
    CHECK_EQ(store.user_metadata().size(), std::size_t{2});
    CHECK_EQ(store.user_metadata()[0].second, std::string{"Beta"});

    store.set_consent(false);
    store.set_consent(true);
    CHECK(store.has_consent());
    CHECK(store.consent());
}

TEST_CASE("preinit", "an empty metadata replace is a remembered clear") {
    PreInitStore store;
    store.set_user_metadata(attrs({{"displayName", "Alpha"}}));
    store.set_user_metadata({});  // replace-with-nothing = clear, still replayed
    CHECK(store.has_user_metadata());
    CHECK(store.user_metadata().empty());
}

TEST_CASE("preinit", "start session is a one-way latch that survives until clear") {
    PreInitStore store;
    CHECK(!store.start_session_requested());
    store.request_start_session();
    store.request_start_session();  // idempotent
    CHECK(store.start_session_requested());
}

TEST_CASE("preinit", "clear resets every remembered dimension") {
    PreInitStore store;
    store.add_breadcrumb(2, "boot");
    CHECK(store.add_event("evt", {}));
    CHECK(store.add_metric("met", 1.0, ""));
    store.set_user("u", "s");
    store.set_environment("dev");
    store.set_user_metadata(attrs({{"k", "v"}}));
    store.set_consent(false);
    store.request_start_session();

    store.clear();

    CHECK(store.breadcrumbs().empty());
    CHECK(store.tracks().empty());
    CHECK(!store.has_user());
    CHECK(!store.has_environment());
    CHECK(!store.has_user_metadata());
    CHECK(!store.has_consent());
    CHECK(store.consent());  // back to the default
    CHECK(!store.start_session_requested());
    // The store is reusable after clear (a shutdown -> re-init cycle).
    store.add_breadcrumb(1, "again");
    CHECK_EQ(store.breadcrumbs().size(), std::size_t{1});
}
