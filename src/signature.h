#ifndef TOMBSTONE_SRC_SIGNATURE_H
#define TOMBSTONE_SRC_SIGNATURE_H

#include <cstdint>
#include <string>
#include <string_view>

namespace tombstone {

/** Lowercase hex SHA-256 of `data` (64 chars). Dependency-free implementation. */
std::string sha256_hex(std::string_view data);

/**
 * Lowercase hex HMAC-SHA256 of `message` keyed by `key` (64 chars), per RFC 2104,
 * built on the local FIPS 180-4 SHA-256 above (no third-party crypto). Used to sign
 * ingest POSTs (S3) — the key is the per-game ingest token. Never throws.
 */
std::string hmac_sha256_hex(std::string_view key, std::string_view message);

/**
 * Build the `X-Tombstone-Signature` header VALUE for an ingest POST (S3):
 * `"t=<unix_seconds>,v1=<hex>"` where hex = hmac_sha256_hex(ingest_key, "<t>.<body>").
 * Binds the raw body to the timestamp (replay window). Pure (time injected) so it is
 * testable; the transport supplies the current second at send time. Never throws.
 */
std::string build_ingest_signature_header(std::string_view ingest_key, std::string_view body,
                                          std::int64_t unix_seconds);

/**
 * Stable crash fingerprint, mirroring the Unity SDK: SHA-256 over the hint
 * plus the first 8 stack frames with per-frame location suffixes (" (at ...")
 * stripped and whitespace trimmed, truncated to 32 hex chars. The same bug
 * therefore hashes the same across builds installed at different paths.
 */
std::string compute_crash_signature(std::string_view stack_hint, std::string_view stack_trace);

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_SIGNATURE_H
