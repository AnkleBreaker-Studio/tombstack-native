#include "platform.h"

#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <ctime>
#include <unistd.h>
#include <uuid/uuid.h>
#elif defined(__linux__)
#include <fstream>
#endif

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

std::string read_machine_id() {
#if defined(_WIN32)
    // The canonical per-install machine GUID. RRF_SUBKEY_WOW6464KEY pins the
    // 64-bit registry view (the KEY_WOW64_64KEY equivalent for RegGetValue),
    // so a 32-bit build reads the same value a 64-bit one does.
    char value[64] = {};
    DWORD size = static_cast<DWORD>(sizeof(value));
    const LSTATUS status =
        RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid",
                     RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, value, &size);
    if (status != ERROR_SUCCESS || value[0] == '\0') {
        return {};
    }
    return std::string{value};  // RegGetValue NUL-terminates REG_SZ reads
#elif defined(__APPLE__)
    // gethostuuid is libc-only (no IOKit link): the same IOPlatformUUID-backed
    // host UUID, without pulling a framework into the build.
    uuid_t uuid = {};
    const struct timespec timeout = {0, 0};
    if (gethostuuid(uuid, &timeout) != 0) {
        return {};
    }
    char text[37] = {};
    uuid_unparse_lower(uuid, text);
    return std::string{text};
#elif defined(__linux__)
    std::ifstream in{"/etc/machine-id"};
    if (!in) {
        return {};
    }
    std::string line;
    std::getline(in, line);
    while (!line.empty() &&
           (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    return line;
#else
    return {};
#endif
}

}  // namespace tombstone
