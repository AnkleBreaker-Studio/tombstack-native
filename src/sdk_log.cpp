#include "sdk_log.h"

namespace tombstone {

void SdkLog::configure(tombstone_log_callback callback, void *user_data) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    callback_ = callback;
    user_data_ = user_data;
}

void SdkLog::emit(tombstone_level level, const std::string &message) const noexcept {
    tombstone_log_callback callback = nullptr;
    void *user_data = nullptr;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        callback = callback_;
        user_data = user_data_;
    }
    if (callback == nullptr) {
        return;
    }
    try {
        callback(static_cast<int>(level), message.c_str(), user_data);
    } catch (...) {
        // A throwing callback must never propagate through the SDK.
    }
}

}  // namespace tombstone
