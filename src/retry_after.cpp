#include "retry_after.h"

namespace tombstone {

namespace {

constexpr bool is_ows(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

}  // namespace

long parse_retry_after_seconds(std::string_view value) noexcept {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && is_ows(value[begin])) {
        ++begin;
    }
    while (end > begin && is_ows(value[end - 1])) {
        --end;
    }
    // Empty, or absurdly long (> 9 digits can only be garbage or an attack —
    // the delay is capped at 300 s anyway, so precision past that is pointless).
    if (begin == end || end - begin > 9) {
        return -1;
    }
    long out = 0;
    for (std::size_t i = begin; i < end; ++i) {
        const char c = value[i];
        if (c < '0' || c > '9') {
            return -1;  // the HTTP-date form (or garbage): fall back to backoff
        }
        out = out * 10 + (c - '0');
    }
    return out;
}

std::chrono::seconds retry_delay(int attempt, long retry_after_seconds) noexcept {
    if (attempt < 1) {
        attempt = 1;
    }
    if (attempt > 5) {
        attempt = 5;  // the worker never exceeds max_attempts; clamp defensively
    }
    long delay = 2L << (attempt - 1);  // 2 s, 4 s, 8 s, 16 s (matches the pre-0.7 backoff)
    if (retry_after_seconds > delay) {
        delay = retry_after_seconds;
    }
    if (delay > max_retry_after_seconds) {
        delay = max_retry_after_seconds;
    }
    return std::chrono::seconds{delay};
}

}  // namespace tombstone
