#ifndef TOMBSTONE_SRC_PLATFORM_H
#define TOMBSTONE_SRC_PLATFORM_H

#include <filesystem>
#include <string>

namespace tombstone {

/** Wire `os` enum value for this build: "windows" | "macos" | "linux" | "other". */
const char *os_name() noexcept;

/** Wire `arch` enum value for this build: "x64" | "arm64" | "x86" | "other". */
const char *arch_name() noexcept;

/**
 * Default writable data directory when the integrator passes none:
 * Windows %LOCALAPPDATA%\Tombstone, macOS ~/Library/Application Support/Tombstone,
 * Linux $XDG_DATA_HOME/tombstone or ~/.local/share/tombstone. Falls back to the
 * current path when no environment hint exists. May throw std::bad_alloc only.
 */
std::filesystem::path default_data_dir();

/**
 * Best-effort stable per-machine identifier, the source for the device-derived
 * provisional user id (v0.8). NEVER sent raw — device_identity::derive hashes
 * it salted with the per-game ingest token. Windows: the
 * HKLM\SOFTWARE\Microsoft\Cryptography MachineGuid (read from the 64-bit
 * registry view). Linux: the first line of /etc/machine-id. macOS: the host
 * UUID via gethostuuid(). Returns "" on any failure or unknown platform — the
 * caller falls back to a random source.
 */
std::string read_machine_id();

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_PLATFORM_H
