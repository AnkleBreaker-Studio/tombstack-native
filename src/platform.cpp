#include "platform.h"

#include <cstdlib>

namespace tombstone {

const char *os_name() noexcept {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "other";
#endif
}

const char *arch_name() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
    return "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return "other";
#endif
}

namespace {

/** getenv as a string; empty when unset (read-only env access, never freed). */
std::string env_or_empty(const char *name) {
    const char *value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
    return value != nullptr ? std::string{value} : std::string{};
}

}  // namespace

std::filesystem::path default_data_dir() {
#if defined(_WIN32)
    const std::string base = env_or_empty("LOCALAPPDATA");
    if (!base.empty()) {
        return std::filesystem::path{base} / "Tombstone";
    }
#elif defined(__APPLE__)
    const std::string home = env_or_empty("HOME");
    if (!home.empty()) {
        return std::filesystem::path{home} / "Library" / "Application Support" / "Tombstone";
    }
#else
    const std::string xdg = env_or_empty("XDG_DATA_HOME");
    if (!xdg.empty()) {
        return std::filesystem::path{xdg} / "tombstone";
    }
    const std::string home = env_or_empty("HOME");
    if (!home.empty()) {
        return std::filesystem::path{home} / ".local" / "share" / "tombstone";
    }
#endif
    return std::filesystem::path{"tombstone-data"};
}

}  // namespace tombstone
