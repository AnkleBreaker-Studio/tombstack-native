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

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_PLATFORM_H
