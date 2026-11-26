#ifndef TOMBSTONE_SRC_STORAGE_OWNER_H
#define TOMBSTONE_SRC_STORAGE_OWNER_H

#include <filesystem>
#include <string>
#include <string_view>

namespace tombstone {

class SdkLog;

/**
 * Which game a data directory belongs to.
 *
 * Up to 0.9.4 every game that left `data_dir` unset shared one directory per OS
 * user, so a game drained, with its own token, the pending reports, session
 * marker, logs, crash dump and identity another game had left there. Storage is
 * now owned:
 *
 *  - the default is a directory of the game's own,
 *    `<platform base>/Tombstone/game-<storage key>` (platform.h); the files an
 *    older SDK left in the shared base itself are never read;
 *  - the owner's storage key is recorded in `<dir>/owner` on first use;
 *  - a directory whose owner file names another key is never used directly:
 *    the game's own `game-<storage key>` subdirectory inside it is used instead.
 */
namespace storage_owner {

inline constexpr const char *kOwnerFileName = "owner";

/** 16 lowercase hex chars: SHA-256 over a fixed label and the SDK token. One-way. */
std::string storage_key(std::string_view token);

/** "game-" + storage_key(token): the per-game directory name. */
std::string game_dir_name(std::string_view token);

/**
 * Create `dir` when needed and claim it for `key`, or fall back to
 * `dir/game-<key>` when another key owns it. A directory without an owner file
 * (a fresh one, or one an SDK wrote before ownership existed) is claimed.
 * Returns the directory to use; empty when none is usable. Never throws.
 */
std::filesystem::path claim(const std::filesystem::path &dir, const std::string &key,
                            SdkLog &log) noexcept;

}  // namespace storage_owner
}  // namespace tombstone

#endif  // TOMBSTONE_SRC_STORAGE_OWNER_H
