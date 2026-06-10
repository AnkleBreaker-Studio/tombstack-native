#ifndef TOMBSTONE_SRC_SIGNATURE_H
#define TOMBSTONE_SRC_SIGNATURE_H

#include <string>
#include <string_view>

namespace tombstone {

/** Lowercase hex SHA-256 of `data` (64 chars). Dependency-free implementation. */
std::string sha256_hex(std::string_view data);

/**
 * Stable crash fingerprint, mirroring the Unity SDK: SHA-256 over the hint
 * plus the first 8 stack frames with per-frame location suffixes (" (at ...")
 * stripped and whitespace trimmed, truncated to 32 hex chars. The same bug
 * therefore hashes the same across builds installed at different paths.
 */
std::string compute_crash_signature(std::string_view stack_hint, std::string_view stack_trace);

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_SIGNATURE_H
