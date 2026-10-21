// SPDX-License-Identifier: Apache-2.0

#include "pcf8563_model.h"

#include <string.h>

// Register addresses and bits (datasheet Rev. 11.1, 8.2-8.6).
#define REG_CONTROL_1 0x00u
#define REG_CONTROL_2 0x01u
#define REG_MINUTE_ALARM 0x09u
#define REG_HOUR_ALARM 0x0Au
#define REG_DAY_ALARM 0x0Bu
#define REG_WEEKDAY_ALARM 0x0Cu
#define REG_SECONDS 0x02u
#define REG_MINUTES 0x03u
#define REG_HOURS 0x04u
#define REG_DAYS 0x05u
#define REG_WEEKDAYS 0x06u
#define REG_MONTHS 0x07u
#define REG_YEARS 0x08u

#define CONTROL_1_TEST1 0x80u
#define CONTROL_1_STOP 0x20u
#define CONTROL_1_RESET 0x08u  // TESTC (Table 27)

#define VL_BIT 0x80u
#define CENTURY_BIT 0x80u
#define ALARM_OFF_BIT 0x80u  // AE_x

// Control_status_2 (Table 6): bits 7-5 always 0; AF and TF cleared by a 0.
#define CONTROL_2_USED 0x1Fu
#define CONTROL_2_AF 0x08u
#define CONTROL_2_TF 0x04u
#define CONTROL_2_AIE 0x02u
#define CONTROL_2_TIE 0x01u

#define SECONDS_MASK 0x7Fu
#define MINUTES_MASK 0x7Fu
#define HOURS_MASK 0x3Fu
#define DAYS_MASK 0x3Fu
#define WEEKDAYS_MASK 0x07u
#define MONTHS_MASK 0x1Fu
#define YEARS_MASK 0xFFu

#define POINTER_MASK 0x0Fu
#define US_PER_MS 1000u

// ASSUMED (pcf8563_model.h): what the time registers hold at power-up.
#define POWER_UP_WEEKDAY 6u  // 2000-01-01 was a Saturday

static pcf8563_model_t *self_of(i2c_model_t *model)
{
    return (pcf8563_model_t *)model->impl;
}

static uint8_t bcd_value(uint8_t bcd)
{
    const uint8_t tens = (uint8_t)(bcd >> 4);
    const uint8_t units = (uint8_t)(bcd & 0x0Fu);
    // ASSUMED: something that is not BCD counts as 0.
    return (tens > 9u || units > 9u) ? 0u : (uint8_t)(tens * 10u + units);
}

static uint8_t to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10u) << 4) | (value % 10u));
}

// Replaces the bits of `reg` under `mask` with `value`'s BCD, keeping the rest.
static void put(pcf8563_model_t *m, uint8_t reg, uint8_t mask, uint8_t value)
{
    m->regs[reg] = (uint8_t)((m->regs[reg] & (uint8_t)~mask) | (to_bcd(value) & mask));
}

// 8.4.4, note [1]: a 29th of February whenever the years register is
// divisible by 4, 00 included.
static uint8_t days_in_month(uint8_t month, uint8_t year)
{
    static const uint8_t k_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1u || month > 12u) {
        return 31u;
    }
    return (month == 2u && year % 4u == 0u) ? 29u : k_days[month - 1u];
}

// One second, with the calendar's carries.
static void tick(pcf8563_model_t *m)
{
    m->stats.ticks++;

    const uint8_t second = bcd_value(m->regs[REG_SECONDS] & SECONDS_MASK);
    if (second < 59u) {
        put(m, REG_SECONDS, SECONDS_MASK, (uint8_t)(second + 1u));
        return;
    }
    put(m, REG_SECONDS, SECONDS_MASK, 0u);

    const uint8_t minute = bcd_value(m->regs[REG_MINUTES] & MINUTES_MASK);
    if (minute < 59u) {
        put(m, REG_MINUTES, MINUTES_MASK, (uint8_t)(minute + 1u));
        return;
    }
    put(m, REG_MINUTES, MINUTES_MASK, 0u);

    const uint8_t hour = bcd_value(m->regs[REG_HOURS] & HOURS_MASK);
    if (hour < 23u) {
        put(m, REG_HOURS, HOURS_MASK, (uint8_t)(hour + 1u));
        return;
    }
    put(m, REG_HOURS, HOURS_MASK, 0u);

    const uint8_t weekday = (uint8_t)(m->regs[REG_WEEKDAYS] & WEEKDAYS_MASK);
    m->regs[REG_WEEKDAYS] =
        (uint8_t)((m->regs[REG_WEEKDAYS] & (uint8_t)~WEEKDAYS_MASK) | ((weekday + 1u) % 7u));

    const uint8_t year = bcd_value(m->regs[REG_YEARS] & YEARS_MASK);
    const uint8_t month = bcd_value(m->regs[REG_MONTHS] & MONTHS_MASK);
    const uint8_t day = bcd_value(m->regs[REG_DAYS] & DAYS_MASK);
    if (day < days_in_month(month, year)) {
        put(m, REG_DAYS, DAYS_MASK, (uint8_t)(day + 1u));
        return;
    }
    put(m, REG_DAYS, DAYS_MASK, 1u);

    if (month >= 1u && month < 12u) {
        put(m, REG_MONTHS, MONTHS_MASK, (uint8_t)(month + 1u));
        return;
    }
    put(m, REG_MONTHS, MONTHS_MASK, 1u);

    if (year < 99u) {
        m->regs[REG_YEARS] = to_bcd((uint8_t)(year + 1u));
        return;
    }
    m->regs[REG_YEARS] = 0x00u;
    m->regs[REG_MONTHS] ^= CENTURY_BIT;  // 8.4.7, note [1]
}

// Every enabled alarm equal to its counter, and at least one enabled.
static bool alarm_matches(const pcf8563_model_t *m)
{
    static const struct {
        uint8_t alarm;
        uint8_t counter;
        uint8_t mask;
    } k_pairs[4] = {
        {REG_MINUTE_ALARM, REG_MINUTES, MINUTES_MASK},
        {REG_HOUR_ALARM, REG_HOURS, HOURS_MASK},
        {REG_DAY_ALARM, REG_DAYS, DAYS_MASK},
        {REG_WEEKDAY_ALARM, REG_WEEKDAYS, WEEKDAYS_MASK},
    };
    bool any = false;
    for (size_t i = 0u; i < sizeof(k_pairs) / sizeof(k_pairs[0]); ++i) {
        const uint8_t alarm = m->regs[k_pairs[i].alarm];
        if ((alarm & ALARM_OFF_BIT) != 0u) {
            continue;
        }
        any = true;
        if ((alarm & k_pairs[i].mask) != (m->regs[k_pairs[i].counter] & k_pairs[i].mask)) {
            return false;
        }
    }
    return any;
}

// INT is low while AF and AIE are set (the timer's half never is): told to
// the callback when it goes low, with the moment.
static void update_int(pcf8563_model_t *m, uint64_t at_us)
{
    const uint8_t c2 = m->regs[REG_CONTROL_2];
    const bool low = ((c2 & CONTROL_2_AF) != 0u && (c2 & CONTROL_2_AIE) != 0u) ||
                     ((c2 & CONTROL_2_TF) != 0u && (c2 & CONTROL_2_TIE) != 0u);
    if (low && !m->int_low) {
        m->stats.int_falls++;
        if (m->on_int != NULL) {
            m->on_int(m->on_int_ctx, at_us);
        }
    }
    m->int_low = low;
}

// After an increment at `at_us`: AF sets if it made the alarms match.
static void look_at_alarm(pcf8563_model_t *m, uint64_t at_us)
{
    const bool matched = alarm_matches(m);
    if (matched && !m->alarm_matched) {
        m->regs[REG_CONTROL_2] |= CONTROL_2_AF;
        m->stats.alarms++;
    }
    m->alarm_matched = matched;
    update_int(m, at_us);
}

bool pcf8563_model_running(const pcf8563_model_t *m)
{
    return (m->regs[REG_CONTROL_1] & (CONTROL_1_STOP | CONTROL_1_TEST1)) == 0u;
}

static uint64_t second_us(const pcf8563_model_t *m)
{
    return (uint64_t)((int64_t)PCF8563_MODEL_SECOND_US - (int64_t)m->ppm);
}

void pcf8563_model_advance(pcf8563_model_t *m, uint64_t now_ms)
{
    if (!pcf8563_model_running(m)) {
        return;
    }
    const uint64_t now_us = now_ms * US_PER_MS;
    while (m->next_tick_us <= now_us) {
        tick(m);
        look_at_alarm(m, m->next_tick_us);
        m->next_tick_us += second_us(m);
    }
}

// A byte written to Control_status_1 at `now_ms`, the counters already
// brought up to it.
static void write_control_1(pcf8563_model_t *m, uint8_t value, uint64_t now_ms)
{
    const bool was_running = pcf8563_model_running(m);
    m->regs[REG_CONTROL_1] = value;
    const bool running = pcf8563_model_running(m);
    if (was_running && !running) {
        m->stats.stops++;
    } else if (!was_running && running) {
        m->stats.starts++;
        m->next_tick_us = now_ms * US_PER_MS + m->first_tick_after_start_us;
    }
}

static hal_status_t model_write(i2c_model_t *model, const uint8_t *d, size_t n, uint64_t now_ms)
{
    pcf8563_model_t *m = self_of(model);
    pcf8563_model_advance(m, now_ms);
    m->stats.writes++;
    if (n == 0u) {
        return HAL_OK;  // an address-only probe
    }
    m->pointer = (uint8_t)(d[0] & POINTER_MASK);
    bool time_written = false;
    for (size_t i = 1u; i < n; ++i) {
        if (m->pointer == REG_CONTROL_1) {
            write_control_1(m, d[i], now_ms);
        } else if (m->pointer == REG_CONTROL_2) {
            // AF and TF: a 0 clears, a 1 leaves them; the rest as written,
            // bits 7-5 always 0 (Table 6).
            const uint8_t flags = (uint8_t)(CONTROL_2_AF | CONTROL_2_TF);
            const uint8_t kept = (uint8_t)(m->regs[REG_CONTROL_2] & d[i] & flags);
            m->regs[REG_CONTROL_2] = (uint8_t)((d[i] & CONTROL_2_USED & (uint8_t)~flags) | kept);
        } else {
            m->regs[m->pointer] = d[i];
            time_written = time_written || (m->pointer >= REG_SECONDS && m->pointer <= REG_YEARS);
        }
        m->pointer = (uint8_t)((m->pointer + 1u) & POINTER_MASK);
    }
    if (time_written) {
        m->stats.time_writes++;
    }
    // A write never sets AF (8.6.5): only what matches now is remembered, so
    // the next increment into a match is the one that does.
    m->alarm_matched = alarm_matches(m);
    update_int(m, now_ms * US_PER_MS);
    return HAL_OK;
}

static hal_status_t model_read(i2c_model_t *model, uint8_t *d, size_t n, uint64_t now_ms)
{
    pcf8563_model_t *m = self_of(model);
    pcf8563_model_advance(m, now_ms);
    m->stats.reads++;
    for (size_t i = 0u; i < n; ++i) {
        d[i] = m->regs[m->pointer];
        m->pointer = (uint8_t)((m->pointer + 1u) & POINTER_MASK);
    }
    return HAL_OK;
}

void pcf8563_model_init(pcf8563_model_t *m, uint64_t now_ms, uint32_t phase_us)
{
    memset(m, 0, sizeof(*m));
    m->model.write = model_write;
    m->model.read = model_read;
    m->model.impl = m;
    m->first_tick_after_start_us =
        (PCF8563_MODEL_FIRST_TICK_MIN_US + PCF8563_MODEL_FIRST_TICK_MAX_US) / 2u;

    m->regs[REG_CONTROL_1] = CONTROL_1_RESET;
    m->regs[REG_CONTROL_2] = 0x00u;             // Table 27
    m->regs[REG_MINUTE_ALARM] = ALARM_OFF_BIT;  // Table 27: AE_x 1, the rest undefined
    m->regs[REG_HOUR_ALARM] = ALARM_OFF_BIT;
    m->regs[REG_DAY_ALARM] = ALARM_OFF_BIT;
    m->regs[REG_WEEKDAY_ALARM] = ALARM_OFF_BIT;
    m->regs[REG_SECONDS] = VL_BIT;  // 00 seconds, VL set (Table 8)
    m->regs[REG_MINUTES] = 0x00u;
    m->regs[REG_HOURS] = 0x00u;
    m->regs[REG_DAYS] = 0x01u;
    m->regs[REG_WEEKDAYS] = POWER_UP_WEEKDAY;
    m->regs[REG_MONTHS] = 0x01u;
    m->regs[REG_YEARS] = 0x00u;

    const uint32_t phase = (phase_us < PCF8563_MODEL_SECOND_US) ? phase_us : 0u;
    m->next_tick_us = now_ms * US_PER_MS + phase;
}

i2c_model_t *pcf8563_model_port(pcf8563_model_t *m)
{
    return &m->model;
}

void pcf8563_model_on_int(pcf8563_model_t *m, pcf8563_model_int_fn fn, void *ctx)
{
    m->on_int = fn;
    m->on_int_ctx = ctx;
}
