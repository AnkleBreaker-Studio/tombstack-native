#include "clock.h"

#include <chrono>
#include <cstdio>

namespace tombstone {

namespace {

constexpr std::int64_t ms_per_day = 86'400'000;

struct CivilDate {
    std::int64_t year;
    unsigned month;
    unsigned day;
};

/** Days-since-epoch to civil date (Howard Hinnant's algorithm, public domain). */
CivilDate civil_from_days(std::int64_t z) noexcept {
    z += 719'468;
    const std::int64_t era = (z >= 0 ? z : z - 146'096) / 146'097;
    const auto doe = static_cast<unsigned>(z - era * 146'097);
    const unsigned yoe = (doe - doe / 1'460 + doe / 36'524 - doe / 146'096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return CivilDate{y + (m <= 2 ? 1 : 0), m, d};
}

}  // namespace

std::string format_iso8601_utc_ms(std::int64_t ms_since_epoch) {
    std::int64_t days = ms_since_epoch / ms_per_day;
    std::int64_t ms_of_day = ms_since_epoch % ms_per_day;
    if (ms_of_day < 0) {
        ms_of_day += ms_per_day;
        days -= 1;
    }
    const CivilDate date = civil_from_days(days);
    const auto hour = static_cast<int>(ms_of_day / 3'600'000);
    const auto minute = static_cast<int>((ms_of_day / 60'000) % 60);
    const auto second = static_cast<int>((ms_of_day / 1'000) % 60);
    const auto millis = static_cast<int>(ms_of_day % 1'000);

    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02d:%02d:%02d.%03dZ",
                  static_cast<long long>(date.year), date.month, date.day, hour, minute, second,
                  millis);
    return std::string{buffer};
}

std::string now_iso8601_utc_ms() {
    const auto now = std::chrono::system_clock::now();
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return format_iso8601_utc_ms(static_cast<std::int64_t>(ms));
}

}  // namespace tombstone
