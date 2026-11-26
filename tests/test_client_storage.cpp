#include "client_harness.h"
#include "device_identity.h"
#include "fake_transport.h"
#include "platform.h"
#include "storage_owner.h"
#include "test_framework.h"

#include <chrono>

namespace fs = std::filesystem;
using client_harness::count_bodies;
using client_harness::count_files;
using client_harness::ScopedEnv;
using client_harness::SdkSession;
using client_harness::write_file;
namespace so = tombstone::storage_owner;

namespace {

constexpr const char *kTokenA = "tmb_test_game_a";
constexpr const char *kTokenB = "tmb_test_game_b";

/** What an SDK up to 0.9.4 leaves in the shared default directory: a pending crash
 *  report, a live session marker, a session log and a device identity. */
void plant_legacy_files(const fs::path &root) {
    write_file(root / "pending" / "crashes" / "1700000000000-aaaaaaaa.json",
               R"({"occurredAtIso":"2026-11-01T00:00:00.000Z","buildVersion":"9.9.9",)"
               R"("os":"windows","arch":"x64","signature":"legacy-other-game",)"
               R"("stackHint":"legacy-other-game","userId":"other-player","log":false})");
    write_file(root / "session.lock",
               R"({"sessionId":"legacy","startedAtIso":"2026-11-01T00:00:00.000Z",)"
               R"("buildVersion":"9.9.9","os":"windows","arch":"x64"})");
    write_file(root / "session.log", "2026-11-01T00:00:00.000Z [Info] other game's log line\n");
    write_file(root / "identity", "dev_0123456789abcdef");
}

}  // namespace

TEST_CASE("client_storage", "the default data directory is a directory of each game's own") {
    ttest::TempDir home;
    const ScopedEnv env{client_harness::kDefaultRootVariable, home.path().string()};
    const fs::path root = tombstone::default_data_root();
    fake_transport::reset();
    { const SdkSession a{kTokenA}; CHECK(a.ok()); }
    { const SdkSession b{kTokenB}; CHECK(b.ok()); }

    const fs::path dir_a = root / so::game_dir_name(kTokenA);
    const fs::path dir_b = root / so::game_dir_name(kTokenB);
    CHECK(dir_a != dir_b);
    CHECK(fs::is_directory(dir_a / "pending"));
    CHECK(fs::is_directory(dir_b / "pending"));
    CHECK(fs::exists(dir_a / so::kOwnerFileName));
    CHECK(!fs::exists(root / "pending"));  // nothing lands in the shared base any more
    CHECK(!fs::exists(root / "identity"));
}

TEST_CASE("client_storage", "what an older SDK left in the shared base is never sent") {
    ttest::TempDir home;
    const ScopedEnv env{client_harness::kDefaultRootVariable, home.path().string()};
    const fs::path root = tombstone::default_data_root();
    plant_legacy_files(root);
    fake_transport::reset();
    {
        const SdkSession b{kTokenB};
        CHECK(b.ok());
        CHECK(tombstone_report_crash("sig-game-b", "game b crash", nullptr, 0) == TOMBSTONE_OK);
        CHECK(tombstone_flush(5000) == TOMBSTONE_OK);
    }
    CHECK_EQ(count_bodies("/ingest/crashes", "legacy-other-game"), std::size_t{0});
    CHECK_EQ(count_bodies("/ingest/crashes", "unclean-shutdown"), std::size_t{0});
    CHECK_EQ(count_bodies("/ingest/crashes", "sig-game-b"), std::size_t{1});
    // B's id is derived for B, not inherited from the identity file left in the base.
    CHECK_EQ(count_bodies("", "dev_0123456789abcdef"), std::size_t{0});
    const std::string machine = tombstone::read_machine_id();
    if (!machine.empty()) {
        const std::string expected = tombstone::device_identity::derive(kTokenB, machine);
        CHECK_EQ(count_bodies("/ingest/crashes", expected), std::size_t{1});
    }
    // The other game's files are left exactly where they were.
    CHECK_EQ(count_files(root / "pending" / "crashes"), std::size_t{1});
    CHECK(fs::exists(root / "session.lock"));
    CHECK(fs::exists(root / "identity"));
}

TEST_CASE("client_storage", "a data_dir another game already uses is not shared with it") {
    ttest::TempDir shared;
    const std::string dir = shared.path().string();
    fake_transport::reset();
    // Game A cannot reach the server: its crash report stays pending in the directory.
    fake_transport::Response offline;
    offline.transport_error = true;
    fake_transport::set_default(offline);
    {
        const SdkSession a{kTokenA, dir};
        CHECK(a.ok());
        CHECK(tombstone_report_crash("sig-game-a", "game a crash", nullptr, 0) == TOMBSTONE_OK);
        (void)tombstone_flush(200);
    }
    CHECK_EQ(count_files(shared.path() / "pending" / "crashes"), std::size_t{1});

    fake_transport::reset();  // everything is accepted from here on
    {
        const SdkSession b{kTokenB, dir};
        CHECK(b.ok());
        CHECK(tombstone_flush(2000) == TOMBSTONE_OK);
    }
    CHECK_EQ(count_bodies("/ingest/crashes", "sig-game-a"), std::size_t{0});
    CHECK(fs::is_directory(shared.path() / so::game_dir_name(kTokenB) / "pending"));

    {
        const SdkSession a{kTokenA, dir};  // game A, back on its own directory, still sends it
        CHECK(a.ok());
        CHECK(tombstone_flush(5000) == TOMBSTONE_OK);
    }
    CHECK_EQ(count_bodies("/ingest/crashes", "sig-game-a"), std::size_t{1});
    CHECK_EQ(count_files(shared.path() / "pending" / "crashes"), std::size_t{0});
}

TEST_CASE("client_storage", "a directory from before ownership existed is claimed by its game") {
    ttest::TempDir legacy;
    // A game that already passed its own data_dir to an older SDK keeps its pending reports.
    write_file(legacy.path() / "pending" / "crashes" / "1700000000000-bbbbbbbb.json",
               R"({"occurredAtIso":"2026-11-01T00:00:00.000Z","buildVersion":"1.0.0",)"
               R"("os":"windows","arch":"x64","signature":"own-pending","stackHint":"h",)"
               R"("log":false})");
    fake_transport::reset();
    {
        const SdkSession a{kTokenA, legacy.path().string()};
        CHECK(a.ok());
        CHECK(tombstone_flush(5000) == TOMBSTONE_OK);
    }
    CHECK_EQ(count_bodies("/ingest/crashes", "own-pending"), std::size_t{1});
    std::ifstream owner{legacy.path() / so::kOwnerFileName};
    std::string recorded;
    std::getline(owner, recorded);
    CHECK_EQ(recorded, so::storage_key(kTokenA));
}
