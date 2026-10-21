// SPDX-License-Identifier: Apache-2.0

#include "pcf8563.h"

#include <stddef.h>
#include <string.h>

// Register addresses (pcf8563.h).
#define REG_CONTROL_STATUS_1 0x00u
#define REG_CONTROL_STATUS_2 0x01u
#define REG_VL_SECONDS 0x02u
#define REG_MINUTE_ALARM 0x09u

// Control_status_2: AIE alone -- AF and TF written 0, which clears them.
#define CONTROL_STATUS_2_AIE 0x02u
#define CONTROL_STATUS_2_OFF 0x00u

// AE_x, bit 7 of each alarm register: set, that alarm is off.
#define ALARM_OFF 0x80u
#define ALARMS 4u  // minute, hour, day, weekday

// Control_status_1's normal value: running, no test mode.
#define CONTROL_STATUS_1_RUN 0x00u
#define CONTROL_STATUS_1_STOP 0x20u

#define VL_BIT 0x80u

// What carries each time field, the rest masked off (pcf8563.h).
#define SECONDS_MASK 0x7Fu
#define MINUTES_MASK 0x7Fu
#define HOURS_MASK 0x3Fu
#define DAYS_MASK 0x3Fu
#define WEEKDAYS_MASK 0x07u
#define MONTHS_MASK 0x1Fu

// Offsets into the burst read of 0x00-0x08.
#define AT_CONTROL_1 0u
#define AT_SECONDS 2u
#define AT_MINUTES 3u
#define AT_HOURS 4u
#define AT_DAYS 5u
#define AT_WEEKDAYS 6u
#define AT_MONTHS 7u
#define AT_YEARS 8u

// CHOICE: how long one transfer may take. A nine-byte read is about 1 ms at
// the board's 100 kHz; 20 ms is the sensor drivers' figure
// (SCD4X_TRANSFER_TIMEOUT_MS, HAC_BME68X_TRANSFER_TIMEOUT_MS), two 10 ms
// scheduler ticks of margin.
#define PCF8563_TRANSFER_TIMEOUT_MS 20u

#define LAST_SECOND 59u
#define LAST_MINUTE 59u
#define LAST_HOUR 23u
#define LAST_WEEKDAY 6u
#define LAST_MONTH 12u
#define LAST_YEAR_IN_CENTURY 99u

// The chip's own leap-year rule (pcf8563.h): every year divisible by 4.
// Right for 2000-2099, the only years it represents.
static uint8_t days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t k_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1u || month > LAST_MONTH) {
        return 0u;
    }
    return (month == 2u && year % 4u == 0u) ? 29u : k_days[month - 1u];
}

// Two BCD digits to binary; false when either nibble is above 9.
static bool from_bcd(uint8_t bcd, uint8_t *out)
{
    const uint8_t tens = (uint8_t)(bcd >> 4);
    const uint8_t units = (uint8_t)(bcd & 0x0Fu);
    if (tens > 9u || units > 9u) {
        return false;
    }
    *out = (uint8_t)(tens * 10u + units);
    return true;
}

static uint8_t to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10u) << 4) | (value % 10u));
}

// Every field of `regs` in range: BCD, and within the calendar.
static bool decode_time(const uint8_t regs[PCF8563_READ_LEN], pcf8563_time_t *out)
{
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    if (!from_bcd((uint8_t)(regs[AT_SECONDS] & SECONDS_MASK), &second) ||
        !from_bcd((uint8_t)(regs[AT_MINUTES] & MINUTES_MASK), &minute) ||
        !from_bcd((uint8_t)(regs[AT_HOURS] & HOURS_MASK), &hour) ||
        !from_bcd((uint8_t)(regs[AT_DAYS] & DAYS_MASK), &day) ||
        !from_bcd((uint8_t)(regs[AT_MONTHS] & MONTHS_MASK), &month) ||
        !from_bcd(regs[AT_YEARS], &year)) {
        return false;
    }
    const uint16_t full_year = (uint16_t)(PCF8563_YEAR_MIN + year);
    if (second > LAST_SECOND || minute > LAST_MINUTE || hour > LAST_HOUR || month < 1u ||
        month > LAST_MONTH || day < 1u || day > days_in_month(full_year, month)) {
        return false;
    }
    out->year = full_year;
    out->month = month;
    out->day = day;
    out->hour = hour;
    out->minute = minute;
    out->second = second;
    out->weekday = (uint8_t)(regs[AT_WEEKDAYS] & WEEKDAYS_MASK);
    return true;
}

void pcf8563_decode(const uint8_t regs[PCF8563_READ_LEN], pcf8563_reading_t *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->regs, regs, PCF8563_READ_LEN);
    out->bus = HAL_OK;
    out->decoded = decode_time(regs, &out->time);

    if ((regs[AT_SECONDS] & VL_BIT) != 0u) {
        out->verdict = PCF8563_TIME_VL_SET;
    } else if ((regs[AT_CONTROL_1] & CONTROL_STATUS_1_STOP) != 0u) {
        out->verdict = PCF8563_TIME_STOPPED;
    } else if (!out->decoded) {
        out->verdict = PCF8563_TIME_UNDECODABLE;
    } else {
        out->verdict = PCF8563_TIME_VALID;
    }
}

void pcf8563_read(port_i2c_t *bus, uint8_t address, pcf8563_reading_t *out)
{
    const uint8_t pointer = REG_CONTROL_STATUS_1;
    uint8_t regs[PCF8563_READ_LEN];

    const hal_status_t status = bus->write_read(bus, address, &pointer, 1u, regs, sizeof(regs),
                                                PCF8563_TRANSFER_TIMEOUT_MS);
    if (status != HAL_OK) {
        memset(out, 0, sizeof(*out));
        out->verdict = PCF8563_TIME_NOT_ANSWERING;
        out->bus = status;
        return;
    }
    pcf8563_decode(regs, out);
}

bool pcf8563_encode(const pcf8563_time_t *utc, uint8_t out[PCF8563_TIME_LEN])
{
    if (utc->year < PCF8563_YEAR_MIN || utc->year > PCF8563_YEAR_MAX || utc->month < 1u ||
        utc->month > LAST_MONTH || utc->day < 1u ||
        utc->day > days_in_month(utc->year, utc->month) || utc->hour > LAST_HOUR ||
        utc->minute > LAST_MINUTE || utc->second > LAST_SECOND || utc->weekday > LAST_WEEKDAY) {
        return false;
    }
    out[0] = to_bcd(utc->second);  // VL clear
    out[1] = to_bcd(utc->minute);
    out[2] = to_bcd(utc->hour);
    out[3] = to_bcd(utc->day);
    out[4] = utc->weekday;
    out[5] = to_bcd(utc->month);  // century bit 0: 20xx
    out[6] = to_bcd((uint8_t)(utc->year - PCF8563_YEAR_MIN));
    return true;
}

static hal_status_t write_control_1(port_i2c_t *bus, uint8_t address, uint8_t value)
{
    const uint8_t control_write[2] = {REG_CONTROL_STATUS_1, value};
    return bus->write(bus, address, control_write, sizeof(control_write),
                      PCF8563_TRANSFER_TIMEOUT_MS);
}

hal_status_t pcf8563_stop(port_i2c_t *bus, uint8_t address)
{
    return write_control_1(bus, address, CONTROL_STATUS_1_STOP);
}

hal_status_t pcf8563_start(port_i2c_t *bus, uint8_t address)
{
    return write_control_1(bus, address, CONTROL_STATUS_1_RUN);
}

hal_status_t pcf8563_write_time(port_i2c_t *bus, uint8_t address, const pcf8563_time_t *utc)
{
    uint8_t time_write[1u + PCF8563_TIME_LEN];
    time_write[0] = REG_VL_SECONDS;
    if (!pcf8563_encode(utc, &time_write[1])) {
        return HAL_ERR_INVALID_ARG;
    }
    return bus->write(bus, address, time_write, sizeof(time_write), PCF8563_TRANSFER_TIMEOUT_MS);
}

hal_status_t pcf8563_alarm_at_minute(port_i2c_t *bus, uint8_t address, uint8_t minute)
{
    if (minute > LAST_MINUTE) {
        return HAL_ERR_INVALID_ARG;
    }
    const uint8_t alarms[1u + ALARMS] = {REG_MINUTE_ALARM, to_bcd(minute), ALARM_OFF, ALARM_OFF,
                                         ALARM_OFF};
    const hal_status_t status =
        bus->write(bus, address, alarms, sizeof(alarms), PCF8563_TRANSFER_TIMEOUT_MS);
    if (status != HAL_OK) {
        return status;
    }
    const uint8_t control[2] = {REG_CONTROL_STATUS_2, CONTROL_STATUS_2_AIE};
    return bus->write(bus, address, control, sizeof(control), PCF8563_TRANSFER_TIMEOUT_MS);
}

hal_status_t pcf8563_alarm_off(port_i2c_t *bus, uint8_t address)
{
    const uint8_t control[2] = {REG_CONTROL_STATUS_2, CONTROL_STATUS_2_OFF};
    const hal_status_t status =
        bus->write(bus, address, control, sizeof(control), PCF8563_TRANSFER_TIMEOUT_MS);
    if (status != HAL_OK) {
        return status;
    }
    const uint8_t alarm[2] = {REG_MINUTE_ALARM, ALARM_OFF};
    return bus->write(bus, address, alarm, sizeof(alarm), PCF8563_TRANSFER_TIMEOUT_MS);
}

hal_status_t pcf8563_set(port_i2c_t *bus, uint8_t address, const pcf8563_time_t *utc)
{
    uint8_t unused[PCF8563_TIME_LEN];
    if (!pcf8563_encode(utc, unused)) {
        return HAL_ERR_INVALID_ARG;
    }

    // Running first, so a clock some earlier firmware stopped starts from the
    // time written next rather than staying frozen at it.
    const hal_status_t status = pcf8563_start(bus, address);
    if (status != HAL_OK) {
        return status;
    }
    return pcf8563_write_time(bus, address, utc);
}

const char *pcf8563_verdict_name(pcf8563_verdict_t verdict)
{
    switch (verdict) {
        case PCF8563_TIME_VALID:
            return "valid";
        case PCF8563_TIME_NOT_ANSWERING:
            return "not answering";
        case PCF8563_TIME_VL_SET:
            return "VL set";
        case PCF8563_TIME_STOPPED:
            return "stopped";
        case PCF8563_TIME_UNDECODABLE:
            return "undecodable";
    }
    return "?";
}
