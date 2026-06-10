#ifndef TOMBSTONE_SRC_SDK_LOG_H
#define TOMBSTONE_SRC_SDK_LOG_H

#include <tombstone/tombstone.h>

#include <mutex>
#include <string>

namespace tombstone {

/**
 * The SDK's own diagnostics channel. Forwards to the integrator's
 * tombstone_log_callback when one was supplied and is silent otherwise —
 * the SDK never writes to stdout/stderr. Thread-safe; callback exceptions
 * are swallowed (the C callback contract is noexcept by convention, but a
 * hostile callback must not take the SDK down).
 */
class SdkLog {
public:
    SdkLog() = default;

    void configure(tombstone_log_callback callback, void *user_data) noexcept;

    void debug(const std::string &message) const noexcept { emit(TOMBSTONE_LEVEL_DEBUG, message); }
    void info(const std::string &message) const noexcept { emit(TOMBSTONE_LEVEL_INFO, message); }
    void warn(const std::string &message) const noexcept { emit(TOMBSTONE_LEVEL_WARN, message); }
    void error(const std::string &message) const noexcept { emit(TOMBSTONE_LEVEL_ERROR, message); }

private:
    void emit(tombstone_level level, const std::string &message) const noexcept;

    mutable std::mutex mutex_;
    tombstone_log_callback callback_{nullptr};
    void *user_data_{nullptr};
};

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_SDK_LOG_H
