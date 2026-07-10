#include "device_identity.h"

#include <cstddef>

namespace tombstone {
namespace device_identity {

namespace {

constexpr std::uint64_t fnv_offset_basis = 14695981039346656037ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;
constexpr std::string_view id_prefix{"dev_"};
// Plausibility bound for a persisted id: "dev_" + 16 hex is 20 chars; anything
// beyond 40 is a corrupt/tampered identity file (mirrors the Unity SDK bound).
constexpr std::size_t max_id_length = 40;

std::uint64_t fnv1a64_append(std::uint64_t hash, std::string_view bytes) noexcept {
    for (const char c : bytes) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= fnv_prime;
    }
    return hash;
}

}  // namespace

std::uint64_t fnv1a64(std::string_view bytes) noexcept {
    return fnv1a64_append(fnv_offset_basis, bytes);
}

std::string derive(const std::string &salt, const std::string &machine_source) {
    // Hash the three pieces in sequence — identical to hashing the concatenated
    // string salt + "|" + machine_source without building it.
    std::uint64_t hash = fnv_offset_basis;
    hash = fnv1a64_append(hash, salt);
    hash = fnv1a64_append(hash, "|");
    hash = fnv1a64_append(hash, machine_source);

    static constexpr char digits[] = "0123456789abcdef";
    std::string out{id_prefix};
    out.reserve(id_prefix.size() + 16);
    for (int shift = 60; shift >= 0; shift -= 4) {
        out += digits[static_cast<std::size_t>((hash >> static_cast<unsigned>(shift)) & 0xFULL)];
    }
    return out;
}

bool is_valid(std::string_view id) noexcept {
    if (id.size() <= id_prefix.size() || id.size() > max_id_length) {
        return false;  // too short to carry a payload past "dev_", or implausibly long
    }
    if (id.substr(0, id_prefix.size()) != id_prefix) {
        return false;
    }
    for (const char c : id.substr(id_prefix.size())) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

}  // namespace device_identity
}  // namespace tombstone
