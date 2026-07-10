#include "device_identity.h"
#include "test_framework.h"

#include <cstdint>
#include <cstdio>
#include <string>

using tombstone::device_identity::derive;
using tombstone::device_identity::fnv1a64;
using tombstone::device_identity::is_valid;

namespace {

/** Independent hex formatting (snprintf) so the derive() tie-in below does not
 *  reuse the implementation's own nibble loop. */
std::string hex16(std::uint64_t value) {
    char buffer[17] = {};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return std::string{buffer};
}

}  // namespace

TEST_CASE("device_identity", "fnv1a64 matches the published FNV vectors") {
    // Offset basis: the hash of the empty input IS the basis.
    CHECK_EQ(fnv1a64(""), std::uint64_t{14695981039346656037ULL});
    // Published FNV-1a 64 test vectors (Landon Curt Noll's reference set).
    CHECK_EQ(fnv1a64("a"), std::uint64_t{0xaf63dc4c8601ec8cULL});
    CHECK_EQ(fnv1a64("foobar"), std::uint64_t{0x85944171f73967e8ULL});
}

TEST_CASE("device_identity", "derive is dev_ + 16 lowercase hex of fnv1a64(salt|source)") {
    // Tie the formatted id to the vector-pinned primitive through an
    // independent snprintf hex path: hashing "tmb_abc|machine-1" must yield
    // the same bytes whether concatenated or fed piecewise.
    const std::string id = derive("tmb_abc", "machine-1");
    CHECK_EQ(id, std::string{"dev_"} + hex16(fnv1a64("tmb_abc|machine-1")));
    CHECK_EQ(id.size(), std::size_t{20});
    CHECK_EQ(id.substr(0, 4), std::string{"dev_"});
    for (const char c : id.substr(4)) {
        CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));  // lowercase hex only
    }
}

TEST_CASE("device_identity", "derive is deterministic") {
    CHECK_EQ(derive("tmb_abc", "machine-1"), derive("tmb_abc", "machine-1"));
    CHECK_EQ(derive("", ""), derive("", ""));
}

TEST_CASE("device_identity", "different salts or sources yield different ids") {
    // Salt separation: the SAME machine must get a DIFFERENT id per game token
    // (cross-tenant unlinkability), and different machines must differ too.
    CHECK(derive("tmb_game_a", "machine-1") != derive("tmb_game_b", "machine-1"));
    CHECK(derive("tmb_game_a", "machine-1") != derive("tmb_game_a", "machine-2"));
}

TEST_CASE("device_identity", "every derived id validates") {
    CHECK(is_valid(derive("tmb_abc", "machine-1")));
    CHECK(is_valid(derive("", "")));  // degenerate inputs still form a valid id
}

TEST_CASE("device_identity", "is_valid accept/reject table") {
    // Accepted: the canonical shape plus the tolerated persisted variants.
    CHECK(is_valid("dev_0123456789abcdef"));        // canonical: dev_ + 16 hex
    CHECK(is_valid("dev_a"));                       // short but plausible
    CHECK(is_valid("dev_abc-123_xyz"));             // '-' and '_' are allowed
    CHECK(is_valid("dev_" + std::string(36, 'a'))); // exactly 40 chars total

    // Rejected: everything a corrupt/tampered identity file could hold.
    CHECK(!is_valid(""));                            // empty
    CHECK(!is_valid("dev_"));                        // bare prefix, no payload
    CHECK(!is_valid("user-123"));                    // no dev_ prefix
    CHECK(!is_valid("DEV_0123456789abcdef"));        // wrong-case prefix
    CHECK(!is_valid("dev_" + std::string(37, 'a'))); // 41 chars: over the bound
    CHECK(!is_valid("dev_ABCDEF"));                  // uppercase remainder
    CHECK(!is_valid("dev_abc def"));                 // space
    CHECK(!is_valid("dev_abc.def"));                 // '.' not in [a-z0-9_-]
    CHECK(!is_valid("dev_abc\ndef"));                // control char
}
