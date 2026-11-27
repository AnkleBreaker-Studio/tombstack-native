#include "platform.h"

#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef RRF_SUBKEY_WOW6464KEY
#define RRF_SUBKEY_WOW6464KEY 0x00010000  // winreg.h value; missing from MinGW-w64 headers
#endif
#elif defined(__APPLE__)
#include <TargetConditionals.h>
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
#elif defined(__ANDROID__)
    return "android";  // checked before __linux__, which Android also defines
#elif defined(__APPLE__)
#if defined(TARGET_OS_MACCATALYST) && TARGET_OS_MACCATALYST
    return "macos";  // an iPad app running on a Mac
#elif defined(TARGET_OS_IOS) && TARGET_OS_IOS
    return "ios";
#elif defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    return "other";  // tvOS / watchOS / visionOS: not in the server's OS enum
#else
    return "macos";
#endif
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

#if defined(_WIN32)
/** An environment variable as a path, read with the wide API: getenv() converts
 *  to the ANSI code page, which turns a user profile name outside it into '?'
 *  and leaves an unusable directory. Empty when unset. */
std::filesystem::path wide_env_path(const wchar_t *name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    value.resize(written);
    return std::filesystem::path{value};
}
#elif !defined(__ANDROID__)
/** getenv as a string; empty when unset (read-only env access, never freed). */
std::string env_or_empty(const char *name) {
    const char *value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
    return value != nullptr ? std::string{value} : std::string{};
}
#endif

}  // namespace

std::filesystem::path default_data_root() {
#if defined(__ANDROID__)
    // No HOME or XDG directory an app can write to, and the working directory is
    // "/": there is no usable default. The game must pass options.data_dir.
    return {};
#else
#if defined(_WIN32)
    const std::filesystem::path base = wide_env_path(L"LOCALAPPDATA");
    if (!base.empty()) {
        return base / "Tombstone";
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
#endif
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
#elif defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    // iOS and its family offer an app no host identifier: the SDK derives the id
    // from a random source instead, kept in data_dir like any other.
    return {};
#elif defined(__APPLE__)
    // gethostuuid is libc-only (no IOKit link): the same IOPlatformUUID-backed
    // host UUID, without pulling a framework into the build. A zero timeout would
    // mean "wait indefinitely", inside tombstone_init: bound it.
    uuid_t uuid = {};
    const struct timespec timeout = {2, 0};
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
