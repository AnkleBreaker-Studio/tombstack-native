#include "storage_owner.h"

#include "sdk_log.h"
#include "signature.h"

#include <fstream>

namespace fs = std::filesystem;

namespace tombstone {
namespace storage_owner {

namespace {

constexpr std::size_t kStorageKeyLength = 16;

std::string read_owner(const fs::path &dir) {
    std::ifstream in{dir / kOwnerFileName, std::ios::binary};
    if (!in) {
        return {};
    }
    std::string owner;
    std::getline(in, owner);
    while (!owner.empty() && (owner.back() == '\r' || owner.back() == '\n' || owner.back() == ' ')) {
        owner.pop_back();
    }
    return owner;
}

bool write_owner(const fs::path &dir, const std::string &key) {
    std::ofstream out{dir / kOwnerFileName, std::ios::binary | std::ios::trunc};
    if (!out) {
        return false;
    }
    out << key << '\n';
    out.close();
    return !out.fail();
}

/** True when `dir` (existing) is, or has just become, owned by `key`. */
bool owned_by(const fs::path &dir, const std::string &key, SdkLog &log) {
    const std::string owner = read_owner(dir);
    if (owner == key) {
        return true;
    }
    if (!owner.empty()) {
        return false;
    }
    if (!write_owner(dir, key)) {
        // An unwritable directory: storage setup degrades on its own right after.
        log.warn("could not record which game owns the data directory");
        return true;
    }
    return read_owner(dir) == key;  // a game claiming it at the same moment wins once only
}

bool ensure_directory(const fs::path &dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    return fs::is_directory(dir, ec);
}

}  // namespace

std::string storage_key(std::string_view token) {
    std::string material{"tombstack-storage-v1\n"};
    material.append(token.data(), token.size());
    return sha256_hex(material).substr(0, kStorageKeyLength);
}

std::string game_dir_name(std::string_view token) { return "game-" + storage_key(token); }

fs::path claim(const fs::path &dir, const std::string &key, SdkLog &log) noexcept {
    try {
        if (!ensure_directory(dir)) {
            return {};
        }
        if (owned_by(dir, key, log)) {
            return dir;
        }
        log.warn("data_dir is used by another game; using this game's own subdirectory in it");
        const fs::path own = dir / ("game-" + key);
        if (!ensure_directory(own) || !owned_by(own, key, log)) {
            return {};
        }
        return own;
    } catch (...) {
        return {};
    }
}

}  // namespace storage_owner
}  // namespace tombstone
