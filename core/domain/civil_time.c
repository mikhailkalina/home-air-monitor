// SPDX-License-Identifier: Apache-2.0

#include "domain/civil_time.h"

#include <stddef.h>

// The Gregorian calendar repeats every 400 years, which hold 146,097 days;
// day 0 of the algorithms below is 0000-03-01, 719,468 days before the epoch.
// Starting the year in March puts the leap day at its end.
#define DAYS_PER_ERA 146097
#define YEARS_PER_ERA 400
#define EPOCH_FROM_0000_03_01 719468
#define EPOCH_WEEKDAY 4  // 1970-01-01 was a Thursday
#define DAYS_PER_WEEK 7

#define SECONDS_PER_MINUTE 60
#define MINUTES_PER_HOUR 60
#define HOURS_PER_DAY 24

// Floor division and modulo, for days and milliseconds before the epoch.
static int64_t floor_div(int64_t a, int64_t b)
{
    const int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

static int64_t floor_mod(int64_t a, int64_t b)
{
    return a - floor_div(a, b) * b;
}

// --- the calendar ------------------------------------------------------------

bool civil_is_leap_year(int32_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

uint8_t civil_days_in_month(int32_t year, uint8_t month)
{
    static const uint8_t k_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1u || month > 12u) {
        return 0u;
    }
    if (month == 2u && civil_is_leap_year(year)) {
        return 29u;
    }
    return k_days[month - 1u];
}

// Howard Hinnant, "chrono-Compatible Low-Level Date Algorithms",
// days_from_civil(): the year runs March to February, so the month's offset
// within it is a linear formula, (153 * m' + 2) / 5.
int64_t civil_days_from_date(int32_t year, uint8_t month, uint8_t day)
{
    const int64_t y = (int64_t)year - ((month <= 2u) ? 1 : 0);
    const int64_t era = floor_div(y, YEARS_PER_ERA);
    const int64_t yoe = y - era * YEARS_PER_ERA;                                // [0, 399]
    const int64_t mp = (month > 2u) ? (int64_t)month - 3 : (int64_t)month + 9;  // March = 0
    const int64_t doy = (153 * mp + 2) / 5 + (int64_t)day - 1;                  // [0, 365]
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                  // [0, 146096]
    return era * DAYS_PER_ERA + doe - EPOCH_FROM_0000_03_01;
}

// The same paper's civil_from_days().
void civil_date_from_days(int64_t days, int32_t *year, uint8_t *month, uint8_t *day)
{
    const int64_t z = days + EPOCH_FROM_0000_03_01;
    const int64_t era = floor_div(z, DAYS_PER_ERA);
    const int64_t doe = z - era * DAYS_PER_ERA;                                 // [0, 146096]
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                // [0, 365]
    const int64_t mp = (5 * doy + 2) / 153;                                     // [0, 11]
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;                             // [1, 31]
    const int64_t m = (mp < 10) ? mp + 3 : mp - 9;                              // [1, 12]

    *year = (int32_t)(yoe + era * YEARS_PER_ERA + ((m <= 2) ? 1 : 0));
    *month = (uint8_t)m;
    *day = (uint8_t)d;
}

uint8_t civil_weekday_from_days(int64_t days)
{
    return (uint8_t)floor_mod(days + EPOCH_WEEKDAY, DAYS_PER_WEEK);
}

// --- instants ----------------------------------------------------------------

bool civil_time_is_valid(const civil_time_t *t)
{
    if (t->year < CIVIL_YEAR_MIN || t->year > CIVIL_YEAR_MAX) {
        return false;
    }
    if (t->month < 1u || t->month > 12u) {
        return false;
    }
    if (t->day < 1u || t->day > civil_days_in_month(t->year, t->month)) {
        return false;
    }
    return t->hour < HOURS_PER_DAY && t->minute < MINUTES_PER_HOUR &&
           t->second < SECONDS_PER_MINUTE;
}

// Any instant, before the epoch included: local time near it can be.
static civil_time_t from_signed_ms(int64_t ms)
{
    civil_time_t t;
    const int64_t days = floor_div(ms, CIVIL_MS_PER_DAY);
    const int64_t ms_of_day = ms - days * CIVIL_MS_PER_DAY;
    const int64_t seconds = ms_of_day / CIVIL_MS_PER_SECOND;

    civil_date_from_days(days, &t.year, &t.month, &t.day);
    t.hour = (uint8_t)(seconds / (SECONDS_PER_MINUTE * MINUTES_PER_HOUR));
    t.minute = (uint8_t)((seconds / SECONDS_PER_MINUTE) % MINUTES_PER_HOUR);
    t.second = (uint8_t)(seconds % SECONDS_PER_MINUTE);
    t.weekday = civil_weekday_from_days(days);
    return t;
}

civil_time_t civil_time_from_unix_ms(uint64_t unix_ms)
{
    return from_signed_ms((int64_t)unix_ms);
}

bool civil_time_to_unix_ms(const civil_time_t *t, uint64_t *out)
{
    if (!civil_time_is_valid(t)) {
        return false;
    }
    const int64_t days = civil_days_from_date(t->year, t->month, t->day);
    const int64_t seconds =
        ((int64_t)t->hour * MINUTES_PER_HOUR + (int64_t)t->minute) * SECONDS_PER_MINUTE +
        (int64_t)t->second;
    *out = (uint64_t)(days * CIVIL_MS_PER_DAY + seconds * CIVIL_MS_PER_SECOND);
    return true;
}

// `n` decimal digits at `text`, or -1 if any of them is not a digit.
static int32_t read_digits(const char *text, size_t n)
{
    int32_t value = 0;
    for (size_t i = 0u; i < n; ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') {
            return -1;
        }
        value = value * 10 + (int32_t)(c - '0');
    }
    return value;
}

bool civil_time_parse_iso8601_utc(const char *text, civil_time_t *out)
{
    // Positions in "YYYY-MM-DDTHH:MM:SSZ".
    static const struct {
        size_t at;
        char c;
    } k_separators[] = {{4u, '-'}, {7u, '-'}, {10u, 'T'}, {13u, ':'}, {16u, ':'}, {19u, 'Z'}};

    if (text == NULL) {
        return false;
    }
    size_t len = 0u;
    while (len <= CIVIL_ISO8601_UTC_LEN && text[len] != '\0') {
        ++len;
    }
    if (len != CIVIL_ISO8601_UTC_LEN) {
        return false;
    }
    for (size_t i = 0u; i < sizeof(k_separators) / sizeof(k_separators[0]); ++i) {
        if (text[k_separators[i].at] != k_separators[i].c) {
            return false;
        }
    }

    const int32_t year = read_digits(&text[0], 4u);
    const int32_t month = read_digits(&text[5], 2u);
    const int32_t day = read_digits(&text[8], 2u);
    const int32_t hour = read_digits(&text[11], 2u);
    const int32_t minute = read_digits(&text[14], 2u);
    const int32_t second = read_digits(&text[17], 2u);
    if (year < 0 || month < 0 || day < 0 || hour < 0 || minute < 0 || second < 0) {
        return false;
    }

    civil_time_t t = {
        .year = year,
        .month = (uint8_t)month,
        .day = (uint8_t)day,
        .hour = (uint8_t)hour,
        .minute = (uint8_t)minute,
        .second = (uint8_t)second,
        .weekday = 0u,
    };
    if (!civil_time_is_valid(&t)) {
        return false;
    }
    t.weekday = civil_weekday_from_days(civil_days_from_date(t.year, t.month, t.day));
    *out = t;
    return true;
}

// --- zones -------------------------------------------------------------------

#define EU_DST_MONTH_START 3u  // March
#define EU_DST_MONTH_END 10u   // October
#define EU_DST_UTC_MINUTE 60   // 01:00 UTC
#define EU_DST_EXTRA_MIN 60    // an hour ahead

time_zone_t time_zone_fixed(int32_t offset_min)
{
    const time_zone_t zone = {
        .standard_offset_min = offset_min,
        .dst_extra_min = 0,
        .dst_start = {0u, 0u, 0u, 0},
        .dst_end = {0u, 0u, 0u, 0},
    };
    return zone;
}

time_zone_t time_zone_eu(int32_t standard_offset_min)
{
    const time_zone_t zone = {
        .standard_offset_min = standard_offset_min,
        .dst_extra_min = EU_DST_EXTRA_MIN,
        .dst_start = {EU_DST_MONTH_START, TIME_ZONE_LAST_WEEK, CIVIL_SUNDAY, EU_DST_UTC_MINUTE},
        .dst_end = {EU_DST_MONTH_END, TIME_ZONE_LAST_WEEK, CIVIL_SUNDAY, EU_DST_UTC_MINUTE},
    };
    return zone;
}

// The instant of `tr` in `year`, in milliseconds since the epoch.
static int64_t transition_ms(int32_t year, const time_zone_transition_t *tr)
{
    uint8_t day;
    if (tr->week >= TIME_ZONE_LAST_WEEK) {
        const uint8_t last = civil_days_in_month(year, tr->month);
        const uint8_t last_wd =
            civil_weekday_from_days(civil_days_from_date(year, tr->month, last));
        day = (uint8_t)(last - (last_wd + DAYS_PER_WEEK - tr->weekday) % DAYS_PER_WEEK);
    } else {
        const uint8_t week = (tr->week == 0u) ? 1u : tr->week;
        const uint8_t first_wd = civil_weekday_from_days(civil_days_from_date(year, tr->month, 1u));
        const int first = 1 + (tr->weekday + DAYS_PER_WEEK - first_wd) % DAYS_PER_WEEK;
        day = (uint8_t)(first + DAYS_PER_WEEK * (week - 1));
    }
    const int64_t days = civil_days_from_date(year, tr->month, day);
    return days * CIVIL_MS_PER_DAY + (int64_t)tr->utc_minute * CIVIL_MS_PER_MINUTE;
}

int32_t time_zone_offset_min(const time_zone_t *zone, uint64_t unix_ms)
{
    if (zone->dst_extra_min == 0) {
        return zone->standard_offset_min;
    }
    const int64_t t = (int64_t)unix_ms;
    const int32_t year = civil_time_from_unix_ms(unix_ms).year;
    const int64_t start = transition_ms(year, &zone->dst_start);
    const int64_t end = transition_ms(year, &zone->dst_end);

    // Northern: summer between the two within the year. Southern: summer
    // from the start to the end of the year, and from its beginning to the
    // end transition.
    const bool summer = (start < end) ? (t >= start && t < end) : (t >= start || t < end);
    return zone->standard_offset_min + (summer ? zone->dst_extra_min : 0);
}

civil_time_t civil_time_local(uint64_t unix_ms, const time_zone_t *zone)
{
    const int64_t offset_ms = (int64_t)time_zone_offset_min(zone, unix_ms) * CIVIL_MS_PER_MINUTE;
    return from_signed_ms((int64_t)unix_ms + offset_ms);
}
