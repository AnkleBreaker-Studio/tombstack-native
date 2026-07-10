#ifndef TOMBSTONE_SRC_DEVICE_IDENTITY_H
#define TOMBSTONE_SRC_DEVICE_IDENTITY_H

#include <cstdint>
#include <string>
#include <string_view>

namespace tombstone {
namespace device_identity {

/**
 * v0.8 device identity (Unity 0.16 parity): the pure half of the persistent
 * device-derived provisional user id ("dev_" + 16 lowercase hex) the SDK
 * acquires at init so no payload ever ships anonymous. The impure halves live
 * elsewhere: read_machine_id() (platform.cpp) supplies the machine source and
 * Client::acquire_device_identity() (client.cpp) handles the identity file.
 */

/** FNV-1a 64-bit over `bytes` (offset basis 14695981039346656037, prime
 *  1099511628211). Exposed so the tests can pin the algorithm to the published
 *  FNV vectors independently of the id formatting. */
std::uint64_t fnv1a64(std::string_view bytes) noexcept;

/**
 * Derive the provisional user id: "dev_" + 16 lowercase hex chars of
 * fnv1a64(salt + "|" + machine_source), most-significant nibble first. The
 * salt is the game's ingest token, so the SAME physical machine yields
 * DIFFERENT ids for different games (cross-tenant unlinkability) and the raw
 * machine identifier never goes on the wire. Deterministic and pure.
 */
std::string derive(const std::string &salt, const std::string &machine_source);

/**
 * Plausibility gate for an id restored from the identity file: it must start
 * with "dev_", have at least one character after the prefix, be at most 40
 * chars total, and the remainder must be [a-z0-9_-] only. A corrupt or
 * hand-edited file fails this and the caller derives a fresh id instead of
 * putting junk on the wire.
 */
bool is_valid(std::string_view id) noexcept;

}  // namespace device_identity
}  // namespace tombstone

#endif  // TOMBSTONE_SRC_DEVICE_IDENTITY_H
