// SPDX-License-Identifier: Apache-2.0

#include "wall_clock.h"

#include <string.h>

#include "domain/civil_time.h"

void wall_clock_read_rtc(port_i2c_t *bus, uint8_t address, uint64_t earliest_ms,
                         wall_clock_rtc_t *out)
{
    memset(out, 0, sizeof(*out));
    pcf8563_read(bus, address, &out->chip);

    if (out->chip.decoded) {
        const civil_time_t t = {
            .year = (int32_t)out->chip.time.year,
            .month = out->chip.time.month,
            .day = out->chip.time.day,
            .hour = out->chip.time.hour,
            .minute = out->chip.time.minute,
            .second = out->chip.time.second,
            .weekday = 0u,
        };
        out->decoded = civil_time_to_unix_ms(&t, &out->unix_ms);
    }
    if (out->chip.verdict != PCF8563_TIME_VALID || !out->decoded) {
        return;
    }
    out->before_earliest = out->unix_ms < earliest_ms;
    out->usable = !out->before_earliest;
}

const char *wall_clock_rtc_reason(const wall_clock_rtc_t *rtc)
{
    switch (rtc->chip.verdict) {
        case PCF8563_TIME_VALID:
            if (!rtc->decoded) {
                break;
            }
            return rtc->before_earliest ? "rtc: implausible year" : "rtc: valid";
        case PCF8563_TIME_NOT_ANSWERING:
            return "rtc: not answering";
        case PCF8563_TIME_VL_SET:
            return "rtc: VL set";
        case PCF8563_TIME_STOPPED:
            return "rtc: stopped";
        case PCF8563_TIME_UNDECODABLE:
            break;
    }
    return "rtc: undecodable";
}

hal_status_t wall_clock_write_rtc(port_i2c_t *bus, uint8_t address, uint64_t unix_ms)
{
    const civil_time_t t = civil_time_from_unix_ms(unix_ms);
    if (t.year < (int32_t)PCF8563_YEAR_MIN || t.year > (int32_t)PCF8563_YEAR_MAX) {
        return HAL_ERR_INVALID_ARG;
    }
    const pcf8563_time_t utc = {
        .year = (uint16_t)t.year,
        .month = t.month,
        .day = t.day,
        .hour = t.hour,
        .minute = t.minute,
        .second = t.second,
        .weekday = t.weekday,
    };
    return pcf8563_set(bus, address, &utc);
}

const char *wall_clock_source_name(wall_clock_source_t source)
{
    switch (source) {
        case WALL_CLOCK_SOURCE_RTC:
            return "rtc";
        case WALL_CLOCK_SOURCE_CONSOLE:
            return "console";
        case WALL_CLOCK_SOURCE_KEPT:
            return "kept across a software reset";
        case WALL_CLOCK_SOURCE_NONE:
            break;
    }
    return "none";
}
