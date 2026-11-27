#include "server_clock.h"

#include "clock.h"

#include <ctime>

namespace tombstone {

namespace {

constexpr std::string_view kDayNames[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
constexpr std::string_view kMonthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
// "Sun, 06 Nov 1994 08:49:37 GMT"
constexpr std::size_t kImfFixdateLength = 29;

constexpr bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/** Parse value[at .. at+count) as decimal digits; false when any is not a digit. */
bool parse_digits(std::string_view value, std::size_t at, std::size_t count, int &out) noexcept {
    int result = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char c = value[at + i];
        if (c < '0' || c > '9') {
            return false;
        }
        result = result * 10 + (c - '0');
    }
    out = result;
    return true;
}

std::int64_t device_now() noexcept { return static_cast<std::int64_t>(std::time(nullptr)); }

}  // namespace

std::optional<std::int64_t> parse_http_date(std::string_view value) noexcept {
    while (!value.empty() && is_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_space(value.back())) {
        value.remove_suffix(1);
    }
    if (value.size() != kImfFixdateLength || value.substr(3, 2) != ", " || value[7] != ' ' ||
        value[11] != ' ' || value[16] != ' ' || value[19] != ':' || value[22] != ':' ||
        value.substr(25) != " GMT") {
        return std::nullopt;
    }
    bool known_day = false;
    for (const std::string_view name : kDayNames) {
        known_day = known_day || value.substr(0, 3) == name;
    }
    unsigned month = 0;
    for (unsigned i = 0; i < 12; ++i) {
        if (value.substr(8, 3) == kMonthNames[i]) {
            month = i + 1;
        }
    }
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!known_day || month == 0 || !parse_digits(value, 5, 2, day) ||
        !parse_digits(value, 12, 4, year) || !parse_digits(value, 17, 2, hour) ||
        !parse_digits(value, 20, 2, minute) || !parse_digits(value, 23, 2, second)) {
        return std::nullopt;
    }
    if (day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
        return std::nullopt;
    }
    const std::int64_t days =
        days_from_civil(year, month, static_cast<unsigned>(day));
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

bool ServerClock::observe(std::string_view date_header, std::int64_t device_now_unix) noexcept {
    const std::optional<std::int64_t> server_now = parse_http_date(date_header);
    if (!server_now || *server_now < min_plausible_unix || *server_now >= max_plausible_unix) {
        return false;
    }
    const std::int64_t observed = *server_now - device_now_unix;
    const std::int64_t current = offset_.load(std::memory_order_relaxed);
    const std::int64_t drift = observed > current ? observed - current : current - observed;
    if (drift < hysteresis_seconds) {
        return false;
    }
    offset_.store(observed, std::memory_order_relaxed);
    return true;
}

bool ServerClock::observe(std::string_view date_header) noexcept {
    return observe(date_header, device_now());
}

std::int64_t ServerClock::offset_seconds() const noexcept {
    return offset_.load(std::memory_order_relaxed);
}

std::int64_t ServerClock::now_unix_seconds(std::int64_t device_now_unix) const noexcept {
    return device_now_unix + offset_seconds();
}

std::int64_t ServerClock::now_unix_seconds() const noexcept {
    return now_unix_seconds(device_now());
}

Unauthorized classify_unauthorized(long status, std::string_view body) noexcept {
    if (status != 401) {
        return Unauthorized::none;
    }
    // Matched quoted, so it can only hit the JSON string value of `code`.
    if (body.find("\"invalid_api_key\"") != std::string_view::npos) {
        return Unauthorized::key;
    }
    return Unauthorized::signature;
}

}  // namespace tombstone
