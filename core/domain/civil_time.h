// SPDX-License-Identifier: Apache-2.0
//
// Civil time: the calendar date and the time of day of an instant given in
// milliseconds since the Unix epoch, in UTC or in a local zone, without
// <time.h> -- the core may not include it, and nothing here needs more than
// integer arithmetic.
//
// The date comes from the number of days since 1970-01-01 (Howard Hinnant's
// days_from_civil and civil_from_days, proleptic Gregorian), so every year
// boundary, leap day and century rule is one formula rather than a table.
//
// Local time is UTC plus an offset that a zone rule gives for the instant.
// The rule is data the composition passes in -- as it passes update_policy's
// config -- and the conversion holds no offset and no date of its own
// (docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md): a standard
// offset, and optionally summer time between two transitions, each "the nth
// (or last) such weekday of such month, at such a time in UTC". The European
// Union's rule is one such value (time_zone_eu()).

#ifndef HAC_CORE_DOMAIN_CIVIL_TIME_H
#define HAC_CORE_DOMAIN_CIVIL_TIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CIVIL_MS_PER_SECOND 1000
#define CIVIL_MS_PER_MINUTE 60000
#define CIVIL_MINUTES_PER_DAY 1440
#define CIVIL_MS_PER_DAY 86400000LL

// civil_time_t.weekday, and time_zone_transition_t.weekday.
#define CIVIL_SUNDAY 0u
#define CIVIL_SATURDAY 6u

// The range civil_time_is_valid() accepts. The lower bound is the epoch:
// nothing here represents an instant before it. The upper bound keeps every
// valid time's milliseconds far inside 64 bits.
#define CIVIL_YEAR_MIN 1970
#define CIVIL_YEAR_MAX 9999

// "YYYY-MM-DDTHH:MM:SSZ": what civil_time_parse_iso8601_utc() reads.
#define CIVIL_ISO8601_UTC_LEN 20u

typedef struct {
    int32_t year;     // e.g. 2026
    uint8_t month;    // 1-12
    uint8_t day;      // 1-31, within the month
    uint8_t hour;     // 0-23
    uint8_t minute;   // 0-59
    uint8_t second;   // 0-59; no leap second
    uint8_t weekday;  // 0 = Sunday ... 6 = Saturday; derived, never read back
} civil_time_t;

// --- the calendar ------------------------------------------------------------

bool civil_is_leap_year(int32_t year);

// 28-31; 0 for a month outside 1-12.
uint8_t civil_days_in_month(int32_t year, uint8_t month);

// Days from 1970-01-01 to the given date, negative before it. The date is
// not checked.
int64_t civil_days_from_date(int32_t year, uint8_t month, uint8_t day);

// The inverse of civil_days_from_date().
void civil_date_from_days(int64_t days, int32_t *year, uint8_t *month, uint8_t *day);

// 0 = Sunday ... 6 = Saturday, of the day `days` after 1970-01-01 (a
// Thursday).
uint8_t civil_weekday_from_days(int64_t days);

// --- instants ----------------------------------------------------------------

// Every field in range, the day within its month, the year within
// CIVIL_YEAR_MIN..CIVIL_YEAR_MAX. The weekday is not checked: it is derived.
bool civil_time_is_valid(const civil_time_t *t);

// The broken-down UTC time of `unix_ms`, weekday included. Milliseconds are
// dropped (rounded down to the second).
civil_time_t civil_time_from_unix_ms(uint64_t unix_ms);

// The instant `t` names, read as UTC; its weekday is ignored. False, leaving
// *out alone, when !civil_time_is_valid(t).
bool civil_time_to_unix_ms(const civil_time_t *t, uint64_t *out);

// Exactly "YYYY-MM-DDTHH:MM:SSZ" -- UTC, so a time typed at the console or
// on a command line has no summer-time ambiguity -- into `out`, weekday
// included. False, leaving *out alone, for anything else: another length,
// another separator, a lowercase 't' or 'z', an offset instead of 'Z', a
// date that does not exist (2026-02-29), a second of 60.
bool civil_time_parse_iso8601_utc(const char *text, civil_time_t *out);

// --- zones -------------------------------------------------------------------

// time_zone_transition_t.week: the last such weekday of the month, however
// many it has.
#define TIME_ZONE_LAST_WEEK 5u

// One change between standard and summer time, in a given year: the
// `week`-th `weekday` of `month` (week 1-4, or TIME_ZONE_LAST_WEEK), at
// `utc_minute` minutes after 00:00 UTC of that day -- negative, or past
// CIVIL_MINUTES_PER_DAY, for a rule whose local time falls on the UTC day
// before or after.
typedef struct {
    uint8_t month;
    uint8_t week;
    uint8_t weekday;
    int32_t utc_minute;
} time_zone_transition_t;

typedef struct {
    int32_t standard_offset_min;  // local standard time minus UTC
    // Added to the standard offset from dst_start until dst_end; 0 means the
    // zone keeps standard time all year, and the transitions are not read.
    // With dst_start later in the year than dst_end, summer time spans the
    // new year (the southern hemisphere's case).
    int32_t dst_extra_min;
    time_zone_transition_t dst_start;
    time_zone_transition_t dst_end;
} time_zone_t;

// A zone that never changes its offset.
time_zone_t time_zone_fixed(int32_t offset_min);

// The European Union's summer time (Directive 2000/84/EC, articles 2 and 3):
// an hour ahead of standard time from 01:00 UTC on the last Sunday of March
// to 01:00 UTC on the last Sunday of October, the same instant in every
// member state. `standard_offset_min` is the state's standard time: 120 for
// Lithuania.
time_zone_t time_zone_eu(int32_t standard_offset_min);

// Local time minus UTC at `unix_ms`, in minutes.
int32_t time_zone_offset_min(const time_zone_t *zone, uint64_t unix_ms);

// The local civil time of `unix_ms` in `zone`, weekday included.
civil_time_t civil_time_local(uint64_t unix_ms, const time_zone_t *zone);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_DOMAIN_CIVIL_TIME_H
