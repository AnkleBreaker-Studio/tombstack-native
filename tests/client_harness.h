#ifndef TOMBSTONE_TESTS_CLIENT_HARNESS_H
#define TOMBSTONE_TESTS_CLIENT_HARNESS_H

// Helpers for the client-level tests (tombstone_client_tests): the SDK runs for
// real through its C ABI, over tests/fake_transport.cpp instead of libcurl.

#include <tombstone/tombstone.h>

#include "fake_transport.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>

namespace client_harness {

inline constexpr const char *kEndpoint = "http://fake.invalid";

/** Environment variable the platform default data root is derived from (platform.cpp). */
#if defined(_WIN32)
inline constexpr const char *kDefaultRootVariable = "LOCALAPPDATA";
#elif defined(__APPLE__)
inline constexpr const char *kDefaultRootVariable = "HOME";
#else
inline constexpr const char *kDefaultRootVariable = "XDG_DATA_HOME";
#endif

/** Sets an environment variable for the scope, restoring the previous value after. */
class ScopedEnv {
public:
    ScopedEnv(const char *name, const std::string &value) : name_(name) {
        const char *previous = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
        had_previous_ = previous != nullptr;
        previous_ = had_previous_ ? previous : "";
        set(value.c_str());
    }
    ~ScopedEnv() {
        if (had_previous_) {
            set(previous_.c_str());
        } else {
#if defined(_WIN32)
            _putenv_s(name_.c_str(), "");
#else
            unsetenv(name_.c_str());
#endif
        }
    }
    ScopedEnv(const ScopedEnv &) = delete;
    ScopedEnv &operator=(const ScopedEnv &) = delete;

private:
    void set(const char *value) {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value);
#else
        setenv(name_.c_str(), value, 1);
#endif
    }
    std::string name_;
    std::string previous_;
    bool had_previous_{false};
};

/** One SDK lifetime: tombstone_init on construction, tombstone_shutdown on destruction
 *  (also when a CHECK throws, so the next test never finds the SDK initialized). */
class SdkSession {
public:
    explicit SdkSession(const char *token, const std::string &data_dir = {},
                        const std::function<void(tombstone_options &)> &tweak = {}) {
        tombstone_options options;
        tombstone_options_init(&options);
        options.endpoint = kEndpoint;
        options.token = token;
        options.build_version = "1.0.0-test";
        options.data_dir = data_dir.empty() ? nullptr : data_dir.c_str();
        if (tweak) {
            tweak(options);
        }
        result_ = tombstone_init(&options);
    }
    ~SdkSession() { tombstone_shutdown(); }
    SdkSession(const SdkSession &) = delete;
    SdkSession &operator=(const SdkSession &) = delete;

    bool ok() const noexcept { return result_ == TOMBSTONE_OK; }

private:
    tombstone_result result_{TOMBSTONE_ERROR_INTERNAL};
};

/** A pending-file name as the SDK writes it, stamped an hour ago: a report an earlier
 *  session left (a stamp older than 90 days would expire unsent). */
inline std::string pending_file_name(std::string_view hex8) {
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch() - std::chrono::hours{1})
                           .count();
    return std::to_string(stamp) + "-" + std::string{hex8} + ".json";
}

inline void write_file(const std::filesystem::path &path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

inline std::size_t count_files(const std::filesystem::path &dir) {
    std::size_t count = 0;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator{dir, ec}) {
        if (entry.is_regular_file(ec)) {
            ++count;
        }
    }
    return count;
}

/** Requests whose URL ends with `url_suffix` and whose body contains `needle`. */
inline std::size_t count_bodies(std::string_view url_suffix, std::string_view needle) {
    std::size_t count = 0;
    for (const fake_transport::Request &request : fake_transport::requests()) {
        const std::string_view url{request.url};
        const bool url_matches = url.size() >= url_suffix.size() &&
                                 url.substr(url.size() - url_suffix.size()) == url_suffix;
        if (url_matches && request.body.find(needle) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

}  // namespace client_harness

#endif  // TOMBSTONE_TESTS_CLIENT_HARNESS_H
