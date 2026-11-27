#include "delivery_policy.h"

#include "clock.h"
#include "json_scan.h"

#include <algorithm>
#include <string>

namespace tombstone {

namespace {

bool is_success(long status) noexcept { return status >= 200 && status < 300; }

/** No answer to act on, or an answer that asks to come back later. */
bool is_transient(bool transport_error, long status) noexcept {
    return transport_error || status == 408 || status == 429 || status >= 500;
}

/** The only answers that condemn the payload itself rather than this launch's sending. */
bool is_malformed_or_too_large(long status) noexcept {
    return status == 400 || status == 413 || status == 422;
}

/** value[at .. at+count) as decimal digits; false when any is not a digit. */
bool read_digits(std::string_view value, std::size_t at, std::size_t count, int &out) noexcept {
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

bool is_leap_year(int year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int month) noexcept {
    static constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (month == 2 && is_leap_year(year)) ? 29 : days[month - 1];
}

}  // namespace

Settle settle_api_response(bool transport_error, long status, Unauthorized unauthorized,
                           bool ingest_post) noexcept {
    if (!transport_error && is_success(status)) {
        return Settle::delivered;
    }
    if (is_transient(transport_error, status)) {
        return Settle::retry;
    }
    if (status == 401 && unauthorized == Unauthorized::signature) {
        return Settle::retry;  // a wrong device clock, corrected from the response's Date
    }
    if (status == 402) {
        return Settle::spend_cap;
    }
    if (is_malformed_or_too_large(status) || !ingest_post) {
        return Settle::drop;
    }
    return Settle::stop_launch;
}

Settle settle_upload_response(bool transport_error, long status) noexcept {
    if (!transport_error && is_success(status)) {
        return Settle::delivered;
    }
    return is_transient(transport_error, status) ? Settle::retry : Settle::drop;
}

std::optional<std::int64_t> parse_iso8601_utc_seconds(std::string_view value) noexcept {
    // "YYYY-MM-DDTHH:MM:SS" then an optional ".fraction", then "Z".
    constexpr std::size_t seconds_end = 19;
    if (value.size() < seconds_end + 1 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' || value.back() != 'Z') {
        return std::nullopt;
    }
    std::string_view fraction = value.substr(seconds_end, value.size() - seconds_end - 1);
    if (!fraction.empty()) {
        if (fraction.front() != '.' || fraction.size() < 2 || fraction.size() > 10) {
            return std::nullopt;
        }
        for (const char c : fraction.substr(1)) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
        }
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!read_digits(value, 0, 4, year) || !read_digits(value, 5, 2, month) ||
        !read_digits(value, 8, 2, day) || !read_digits(value, 11, 2, hour) ||
        !read_digits(value, 14, 2, minute) || !read_digits(value, 17, 2, second)) {
        return std::nullopt;
    }
    if (month < 1 || month > 12 || day < 1 || day > days_in_month(year, month) || hour > 23 ||
        minute > 59 || second > 60) {
        return std::nullopt;
    }
    const std::int64_t days =
        days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    return days * 86'400 + hour * 3'600 + minute * 60 + second;
}

std::int64_t spend_cap_pause_seconds(std::string_view body, std::int64_t server_now_unix,
                                     std::int64_t min_pause) noexcept {
    std::int64_t pause = spend_cap_default_pause_seconds;
    try {
        const std::optional<std::string> resumes_at =
            find_string_field(body.substr(0, spend_cap_max_body_bytes), "resumesAt");
        const std::optional<std::int64_t> resume_unix =
            resumes_at ? parse_iso8601_utc_seconds(*resumes_at) : std::nullopt;
        if (resume_unix) {
            pause = *resume_unix - server_now_unix;
        }
    } catch (...) {
        // An unreadable body keeps the default stop.
    }
    const std::int64_t floor = std::clamp<std::int64_t>(min_pause, 0, spend_cap_max_pause_seconds);
    return std::clamp(pause, floor, spend_cap_max_pause_seconds);
}

}  // namespace tombstone
