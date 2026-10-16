// SPDX-License-Identifier: Apache-2.0
//
// Civil time without <time.h>: the calendar against itself across two
// centuries and against known dates, the ISO 8601 parser at its edges, and
// local time in Vilnius on either side of every change the EU rule makes --
// the minute before and the minute of, in spring and in autumn -- across a
// year end and a leap day.

#include <string.h>

#include "domain/civil_time.h"
#include "hac_test.h"

// Lithuania's standard time, EET (docs/adr/0023-...).
#define VILNIUS_STANDARD_MIN 120

static uint64_t utc_ms(const char *iso)
{
    civil_time_t t;
    uint64_t ms = 0u;
    const bool parsed = civil_time_parse_iso8601_utc(iso, &t);
    HAC_CHECK(parsed);
    if (parsed) {
        HAC_CHECK(civil_time_to_unix_ms(&t, &ms));
    }
    return ms;
}

static bool is(const civil_time_t *t, int32_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi,
               uint8_t s)
{
    return t->year == y && t->month == mo && t->day == d && t->hour == h && t->minute == mi &&
           t->second == s;
}

// --- the calendar ------------------------------------------------------------

static void leap_years_follow_the_gregorian_rule(void)
{
    HAC_CHECK(civil_is_leap_year(2024));
    HAC_CHECK(civil_is_leap_year(2000));  // divisible by 400
    HAC_CHECK(!civil_is_leap_year(1900));
    HAC_CHECK(!civil_is_leap_year(2100));  // divisible by 100, not by 400
    HAC_CHECK(!civil_is_leap_year(2026));

    HAC_CHECK_EQ_INT(civil_days_in_month(2024, 2u), 29);
    HAC_CHECK_EQ_INT(civil_days_in_month(2026, 2u), 28);
    HAC_CHECK_EQ_INT(civil_days_in_month(2100, 2u), 28);
    HAC_CHECK_EQ_INT(civil_days_in_month(2026, 4u), 30);
    HAC_CHECK_EQ_INT(civil_days_in_month(2026, 12u), 31);
    HAC_CHECK_EQ_INT(civil_days_in_month(2026, 0u), 0);
    HAC_CHECK_EQ_INT(civil_days_in_month(2026, 13u), 0);
}

static void known_days_since_the_epoch(void)
{
    HAC_CHECK_EQ_INT(civil_days_from_date(1970, 1u, 1u), 0);
    HAC_CHECK_EQ_INT(civil_days_from_date(1969, 12u, 31u), -1);
    HAC_CHECK_EQ_INT(civil_days_from_date(2000, 3u, 1u), 11017);
    HAC_CHECK_EQ_INT(civil_days_from_date(2026, 1u, 1u), 20454);

    HAC_CHECK_EQ_INT(civil_weekday_from_days(0), 4);  // a Thursday
    HAC_CHECK_EQ_INT(civil_weekday_from_days(-1), 3);
    HAC_CHECK_EQ_INT(civil_weekday_from_days(civil_days_from_date(2026, 10u, 16u)), 5);  // Friday
    HAC_CHECK_EQ_INT(civil_weekday_from_days(civil_days_from_date(2026, 3u, 29u)), CIVIL_SUNDAY);
    HAC_CHECK_EQ_INT(civil_weekday_from_days(civil_days_from_date(2026, 10u, 25u)), CIVIL_SUNDAY);
}

// Every day from 1969-12-01 to 2101-03-01 converts back to itself, and each
// is the day after the one before: no gap and no repeat at any month end,
// year end, leap day or century.
static void every_day_for_two_centuries_round_trips_in_order(void)
{
    const int64_t first = civil_days_from_date(1969, 12u, 1u);
    const int64_t last = civil_days_from_date(2101, 3u, 1u);
    int32_t y = 1969;
    uint8_t m = 12u;
    uint8_t d = 1u;
    bool ok = true;

    for (int64_t days = first; days <= last && ok; ++days) {
        int32_t yy;
        uint8_t mm;
        uint8_t dd;
        civil_date_from_days(days, &yy, &mm, &dd);
        ok = (yy == y && mm == m && dd == d) && civil_days_from_date(y, m, d) == days;

        // The next date, by counting.
        if (d < civil_days_in_month(y, m)) {
            ++d;
        } else if (m < 12u) {
            ++m;
            d = 1u;
        } else {
            ++y;
            m = 1u;
            d = 1u;
        }
    }
    HAC_CHECK(ok);
}

// --- instants ----------------------------------------------------------------

static void instants_break_down_in_utc(void)
{
    civil_time_t t = civil_time_from_unix_ms(0u);
    HAC_CHECK(is(&t, 1970, 1u, 1u, 0u, 0u, 0u));
    HAC_CHECK_EQ_INT(t.weekday, 4);

    t = civil_time_from_unix_ms(951782400000u);  // 2000-02-29T00:00:00Z
    HAC_CHECK(is(&t, 2000, 2u, 29u, 0u, 0u, 0u));

    t = civil_time_from_unix_ms(1767225599999u);  // a millisecond before 2026
    HAC_CHECK(is(&t, 2025, 12u, 31u, 23u, 59u, 59u));
    t = civil_time_from_unix_ms(1767225600000u);
    HAC_CHECK(is(&t, 2026, 1u, 1u, 0u, 0u, 0u));
    HAC_CHECK_EQ_INT(t.weekday, 4);
}

static void a_valid_time_converts_to_milliseconds_and_back(void)
{
    const civil_time_t t = {2026, 10u, 25u, 1u, 0u, 0u, 0u};
    uint64_t ms = 0u;
    HAC_CHECK(civil_time_to_unix_ms(&t, &ms));
    HAC_CHECK_EQ_U64(ms, 1792890000000u);
    const civil_time_t back = civil_time_from_unix_ms(ms);
    HAC_CHECK(is(&back, 2026, 10u, 25u, 1u, 0u, 0u));
    HAC_CHECK_EQ_INT(back.weekday, CIVIL_SUNDAY);
}

static void an_invalid_time_is_refused(void)
{
    const civil_time_t bad[] = {
        {2026, 2u, 29u, 0u, 0u, 0u, 0u},   // not a leap year
        {2100, 2u, 29u, 0u, 0u, 0u, 0u},   // nor is 2100
        {2026, 4u, 31u, 0u, 0u, 0u, 0u},   // April has 30
        {2026, 13u, 1u, 0u, 0u, 0u, 0u},   // no 13th month
        {2026, 0u, 1u, 0u, 0u, 0u, 0u},    // nor a 0th
        {2026, 1u, 0u, 0u, 0u, 0u, 0u},    // nor a day 0
        {2026, 1u, 1u, 24u, 0u, 0u, 0u},   // nor an hour 24
        {2026, 1u, 1u, 0u, 60u, 0u, 0u},   // nor a minute 60
        {2026, 1u, 1u, 0u, 0u, 60u, 0u},   // nor a leap second
        {1969, 12u, 31u, 0u, 0u, 0u, 0u},  // before the epoch
    };
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        uint64_t ms = 42u;
        HAC_CHECK(!civil_time_is_valid(&bad[i]));
        HAC_CHECK(!civil_time_to_unix_ms(&bad[i], &ms));
        HAC_CHECK_EQ_U64(ms, 42u);  // untouched
    }
    const civil_time_t leap = {2024, 2u, 29u, 23u, 59u, 59u, 0u};
    HAC_CHECK(civil_time_is_valid(&leap));
}

// --- ISO 8601 ----------------------------------------------------------------

static void the_parser_reads_exactly_one_form(void)
{
    civil_time_t t;
    HAC_CHECK(civil_time_parse_iso8601_utc("2026-10-25T01:00:00Z", &t));
    HAC_CHECK(is(&t, 2026, 10u, 25u, 1u, 0u, 0u));
    HAC_CHECK_EQ_INT(t.weekday, CIVIL_SUNDAY);
    HAC_CHECK(civil_time_parse_iso8601_utc("2024-02-29T23:59:59Z", &t));
    HAC_CHECK(civil_time_parse_iso8601_utc("1970-01-01T00:00:00Z", &t));

    const char *bad[] = {
        "",
        "2026-10-25T01:00:00",        // no zone: it would be local, and ambiguous
        "2026-10-25T01:00:00+02:00",  // an offset: UTC only
        "2026-10-25T01:00:00Z ",      // trailing text
        "2026-10-25t01:00:00Z",       // lowercase separator
        "2026-10-25T01:00:00z",
        "2026-10-25 01:00:00Z",
        "2026/10/25T01:00:00Z",
        "2026-1O-25T01:00:00Z",  // a letter O
        "+026-10-25T01:00:00Z",
        "2026-02-29T00:00:00Z",  // no such day
        "2026-10-25T24:00:00Z",
        "2026-10-25T23:60:00Z",
        "2026-10-25T23:59:60Z",  // no leap seconds
        "2026-13-01T00:00:00Z",
        "1969-12-31T23:59:59Z",  // before the epoch
        "26-10-25T01:00:00Z",
    };
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        civil_time_t out = {1, 2u, 3u, 4u, 5u, 6u, 0u};
        HAC_CHECK(!civil_time_parse_iso8601_utc(bad[i], &out));
        HAC_CHECK(is(&out, 1, 2u, 3u, 4u, 5u, 6u));  // untouched
    }
    HAC_CHECK(!civil_time_parse_iso8601_utc(NULL, &t));
}

// --- Vilnius: the EU rule at UTC+2 ----------------------------------------------

static civil_time_t vilnius(const char *iso)
{
    const time_zone_t zone = time_zone_eu(VILNIUS_STANDARD_MIN);
    return civil_time_local(utc_ms(iso), &zone);
}

static void spring_forward_skips_from_two_fifty_nine_to_four(void)
{
    civil_time_t t = vilnius("2026-03-29T00:59:59Z");
    HAC_CHECK(is(&t, 2026, 3u, 29u, 2u, 59u, 59u));
    t = vilnius("2026-03-29T01:00:00Z");
    HAC_CHECK(is(&t, 2026, 3u, 29u, 4u, 0u, 0u));

    // The same rule in 2027, whose last Sunday of March is the 28th.
    t = vilnius("2027-03-28T00:59:59Z");
    HAC_CHECK(is(&t, 2027, 3u, 28u, 2u, 59u, 59u));
    t = vilnius("2027-03-28T01:00:00Z");
    HAC_CHECK(is(&t, 2027, 3u, 28u, 4u, 0u, 0u));
    // A week earlier is no change at all.
    t = vilnius("2027-03-21T01:00:00Z");
    HAC_CHECK(is(&t, 2027, 3u, 21u, 3u, 0u, 0u));
}

static void fall_back_repeats_from_three_fifty_nine_to_three(void)
{
    civil_time_t t = vilnius("2026-10-25T00:59:59Z");
    HAC_CHECK(is(&t, 2026, 10u, 25u, 3u, 59u, 59u));
    t = vilnius("2026-10-25T01:00:00Z");
    HAC_CHECK(is(&t, 2026, 10u, 25u, 3u, 0u, 0u));

    // 2027: the last Sunday of October is the 31st, the month's last day.
    t = vilnius("2027-10-31T00:59:59Z");
    HAC_CHECK(is(&t, 2027, 10u, 31u, 3u, 59u, 59u));
    t = vilnius("2027-10-31T01:00:00Z");
    HAC_CHECK(is(&t, 2027, 10u, 31u, 3u, 0u, 0u));
}

static void summer_and_winter_offsets(void)
{
    const time_zone_t zone = time_zone_eu(VILNIUS_STANDARD_MIN);
    HAC_CHECK_EQ_INT(time_zone_offset_min(&zone, utc_ms("2026-07-01T12:00:00Z")), 180);
    HAC_CHECK_EQ_INT(time_zone_offset_min(&zone, utc_ms("2026-01-15T12:00:00Z")), 120);
    HAC_CHECK_EQ_INT(time_zone_offset_min(&zone, utc_ms("2026-12-31T23:00:00Z")), 120);
    HAC_CHECK_EQ_INT(time_zone_offset_min(&zone, utc_ms("2026-10-16T12:00:00Z")), 180);
}

static void the_year_ends_two_hours_before_utc_does(void)
{
    civil_time_t t = vilnius("2026-12-31T21:59:59Z");
    HAC_CHECK(is(&t, 2026, 12u, 31u, 23u, 59u, 59u));
    t = vilnius("2026-12-31T22:00:00Z");
    HAC_CHECK(is(&t, 2027, 1u, 1u, 0u, 0u, 0u));
    HAC_CHECK_EQ_INT(t.weekday, 5);  // 2027 begins on a Friday
}

static void leap_days_come_and_go_in_local_time(void)
{
    civil_time_t t = vilnius("2028-02-28T22:00:00Z");
    HAC_CHECK(is(&t, 2028, 2u, 29u, 0u, 0u, 0u));
    t = vilnius("2028-02-29T21:59:59Z");
    HAC_CHECK(is(&t, 2028, 2u, 29u, 23u, 59u, 59u));
    t = vilnius("2028-02-29T22:00:00Z");
    HAC_CHECK(is(&t, 2028, 3u, 1u, 0u, 0u, 0u));

    // No 29th in 2027, nor in 2100.
    t = vilnius("2027-02-28T22:00:00Z");
    HAC_CHECK(is(&t, 2027, 3u, 1u, 0u, 0u, 0u));
    t = vilnius("2100-02-28T22:00:00Z");
    HAC_CHECK(is(&t, 2100, 3u, 1u, 0u, 0u, 0u));
}

// --- other rules ---------------------------------------------------------------

static void a_fixed_zone_never_changes(void)
{
    const time_zone_t utc = time_zone_fixed(0);
    civil_time_t t = civil_time_local(utc_ms("2026-03-29T01:00:00Z"), &utc);
    HAC_CHECK(is(&t, 2026, 3u, 29u, 1u, 0u, 0u));

    const time_zone_t minus = time_zone_fixed(-90);
    t = civil_time_local(utc_ms("2026-01-01T01:00:00Z"), &minus);
    HAC_CHECK(is(&t, 2025, 12u, 31u, 23u, 30u, 0u));
}

// The nth weekday rather than the last: New York's, second Sunday of March at
// 02:00 EST (07:00 UTC) to first Sunday of November at 02:00 EDT (06:00 UTC).
static void the_nth_weekday_of_a_month(void)
{
    const time_zone_t zone = {
        .standard_offset_min = -300,
        .dst_extra_min = 60,
        .dst_start = {3u, 2u, CIVIL_SUNDAY, 420},
        .dst_end = {11u, 1u, CIVIL_SUNDAY, 360},
    };
    HAC_CHECK_EQ_INT(time_zone_offset_min(&zone, utc_ms("2026-03-08T06:59:59Z")), -300);
    HAC_CHECK_EQ_INT(time_zone_offset_min(&zone, utc_ms("2026-03-08T07:00:00Z")), -240);
    HAC_CHECK_EQ_INT(time_zone_offset_min(&zone, utc_ms("2026-11-01T05:59:59Z")), -240);
    HAC_CHECK_EQ_INT(time_zone_offset_min(&zone, utc_ms("2026-11-01T06:00:00Z")), -300);
}

// Summer across the new year, and transitions on the UTC day before: Sydney,
// AEST +10, from the first Sunday of October at 02:00 AEST to the first
// Sunday of April at 03:00 AEDT -- both 16:00 UTC on the Saturday.
static void a_southern_summer_spans_the_new_year(void)
{
    const time_zone_t zone = {
        .standard_offset_min = 600,
        .dst_extra_min = 60,
        .dst_start = {10u, 1u, CIVIL_SUNDAY, -480},
        .dst_end = {4u, 1u, CIVIL_SUNDAY, -480},
    };
    HAC_CHECK_EQ_INT(time_zone_offset_min(&zone, utc_ms("2026-01-15T00:00:00Z")), 660);
    HAC_CHECK_EQ_INT(time_zone_offset_min(&zone, utc_ms("2026-04-04T15:59:59Z")), 660);
    HAC_CHECK_EQ_INT(time_zone_offset_min(&zone, utc_ms("2026-04-04T16:00:00Z")), 600);
    HAC_CHECK_EQ_INT(time_zone_offset_min(&zone, utc_ms("2026-07-01T00:00:00Z")), 600);
    HAC_CHECK_EQ_INT(time_zone_offset_min(&zone, utc_ms("2026-10-03T15:59:59Z")), 600);
    HAC_CHECK_EQ_INT(time_zone_offset_min(&zone, utc_ms("2026-10-03T16:00:00Z")), 660);
    HAC_CHECK_EQ_INT(time_zone_offset_min(&zone, utc_ms("2026-12-31T23:59:59Z")), 660);
}

int main(void)
{
    HAC_RUN(leap_years_follow_the_gregorian_rule);
    HAC_RUN(known_days_since_the_epoch);
    HAC_RUN(every_day_for_two_centuries_round_trips_in_order);
    HAC_RUN(instants_break_down_in_utc);
    HAC_RUN(a_valid_time_converts_to_milliseconds_and_back);
    HAC_RUN(an_invalid_time_is_refused);
    HAC_RUN(the_parser_reads_exactly_one_form);
    HAC_RUN(spring_forward_skips_from_two_fifty_nine_to_four);
    HAC_RUN(fall_back_repeats_from_three_fifty_nine_to_three);
    HAC_RUN(summer_and_winter_offsets);
    HAC_RUN(the_year_ends_two_hours_before_utc_does);
    HAC_RUN(leap_days_come_and_go_in_local_time);
    HAC_RUN(a_fixed_zone_never_changes);
    HAC_RUN(the_nth_weekday_of_a_month);
    HAC_RUN(a_southern_summer_spans_the_new_year);
    return hac_test_summary();
}
