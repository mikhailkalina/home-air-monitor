// SPDX-License-Identifier: Apache-2.0

#include "gt911_model.h"

#include <string.h>

// The register map (gt911_model.h).
#define REG_COMMAND 0x8040u
#define CMD_SCREEN_OFF 0x05u
#define REG_CONFIG_LAST (GT911_MODEL_CONFIG_FIRST + GT911_MODEL_CONFIG_LEN - 1u)
#define REG_PRODUCT_INFO 0x8140u
#define PRODUCT_INFO_LEN 11u
#define REG_STATUS 0x814Eu
#define REG_POINTS 0x814Fu
#define POINT_LEN 8u
#define STATUS_READY 0x80u

// Offsets into the configuration area.
#define AT_VERSION 0u
#define AT_X_MAX 1u
#define AT_Y_MAX 3u
#define AT_TOUCH_NUMBER 5u
#define AT_MODULE_SWITCH1 6u
#define AT_CHECKSUM 184u  // 0x80FF

#define US_PER_MS 1000u

static uint8_t lo(uint16_t v)
{
    return (uint8_t)(v & 0xFFu);
}

static uint8_t hi(uint16_t v)
{
    return (uint8_t)(v >> 8);
}

void gt911_model_fix_checksum(gt911_model_t *m)
{
    // The programming guide's rule: the bytes before it, summed, plus the
    // checksum, are 0 modulo 256.
    uint8_t sum = 0u;
    for (size_t i = 0u; i < AT_CHECKSUM; ++i) {
        sum = (uint8_t)(sum + m->config[i]);
    }
    m->config[AT_CHECKSUM] = (uint8_t)(0u - sum);
}

static bool answers(gt911_model_t *m, uint64_t now_ms)
{
    if (m->asleep || now_ms < m->answers_from_ms) {
        m->stats.address_nacks++;
        return false;
    }
    return true;
}

static void latch(gt911_model_t *m)
{
    memcpy(m->buffer, m->touching, sizeof(m->buffer));
    m->buffer_count = (uint8_t)m->touching_count;
    m->buffer_ready = true;
    m->stats.reports++;
    m->stats.interrupts++;
    if (m->on_interrupt != NULL) {
        m->on_interrupt(m->on_interrupt_ctx);
    }
}

// A scan with something to say.
static void report(gt911_model_t *m)
{
    if (m->asleep) {
        return;
    }
    if (m->drop_next > 0u) {
        m->drop_next--;
        m->stats.reports_dropped++;
        return;
    }
    if (m->buffer_ready) {
        m->pending = true;  // ASSUMED: held until the host clears the last one
        return;
    }
    latch(m);
}

static uint8_t read_reg(const gt911_model_t *m, uint16_t reg)
{
    if (reg >= GT911_MODEL_CONFIG_FIRST && reg <= REG_CONFIG_LAST) {
        return m->config[reg - GT911_MODEL_CONFIG_FIRST];
    }
    if (reg >= REG_PRODUCT_INFO && reg < REG_PRODUCT_INFO + PRODUCT_INFO_LEN) {
        const uint16_t x = (uint16_t)(m->config[AT_X_MAX] | (m->config[AT_X_MAX + 1u] << 8));
        const uint16_t y = (uint16_t)(m->config[AT_Y_MAX] | (m->config[AT_Y_MAX + 1u] << 8));
        const uint8_t info[PRODUCT_INFO_LEN] = {
            (uint8_t)m->product_id[0],
            (uint8_t)m->product_id[1],
            (uint8_t)m->product_id[2],
            (uint8_t)m->product_id[3],
            lo(m->firmware),
            hi(m->firmware),
            lo(x),
            hi(x),
            lo(y),
            hi(y),
            m->vendor_id,
        };
        return info[reg - REG_PRODUCT_INFO];
    }
    if (reg == REG_STATUS) {
        return m->buffer_ready ? (uint8_t)(STATUS_READY | m->buffer_count) : 0u;
    }
    if (reg >= REG_POINTS && reg < REG_POINTS + GT911_MODEL_MAX_POINTS * POINT_LEN) {
        const size_t index = (size_t)(reg - REG_POINTS) / POINT_LEN;
        const size_t at = (size_t)(reg - REG_POINTS) % POINT_LEN;
        if (!m->buffer_ready || index >= m->buffer_count) {
            return 0u;
        }
        const gt911_model_point_t *p = &m->buffer[index];
        const uint8_t record[POINT_LEN] = {
            p->track_id, lo(p->x), hi(p->x), lo(p->y), hi(p->y), lo(p->size), hi(p->size), 0u,
        };
        return record[at];
    }
    if (reg == REG_COMMAND) {
        return m->command;
    }
    return 0u;
}

static void write_command(gt911_model_t *m, uint8_t value, uint64_t now_ms)
{
    m->command = value;
    if (value != CMD_SCREEN_OFF) {
        return;
    }
    if (m->int_level != GT911_MODEL_INT_HOST_LOW) {
        m->stats.ignored_sleep_commands++;  // ASSUMED: the datasheet requires INT low first
        return;
    }
    m->asleep = true;
    m->slept_at_ms = now_ms;
    m->stats.sleeps++;
}

static void write_status(gt911_model_t *m)
{
    m->stats.status_clears++;
    m->buffer_ready = false;
    m->buffer_count = 0u;
    if (m->pending) {
        m->pending = false;
        latch(m);  // the next scan's report, the newest points
    }
}

static hal_status_t model_write(i2c_model_t *self, const uint8_t *d, size_t n, uint64_t now_ms)
{
    gt911_model_t *m = (gt911_model_t *)self->impl;
    if (!answers(m, now_ms)) {
        return HAL_ERR_NOT_FOUND;
    }
    if (n < 2u) {
        return HAL_OK;  // a probe, or half an address: nothing changes
    }
    m->pointer = (uint16_t)((d[0] << 8) | d[1]);
    for (size_t i = 2u; i < n; ++i) {
        const uint16_t reg = m->pointer;
        if (reg >= GT911_MODEL_CONFIG_FIRST && reg <= REG_CONFIG_LAST) {
            m->stats.config_writes++;  // ASSUMED: refused, so no test can pass by it
            return HAL_ERR_IO;
        }
        if (reg == REG_COMMAND) {
            write_command(m, d[i], now_ms);
        } else if (reg == REG_STATUS) {
            write_status(m);
        }
        m->pointer = (uint16_t)(m->pointer + 1u);
    }
    return HAL_OK;
}

static hal_status_t model_read(i2c_model_t *self, uint8_t *d, size_t n, uint64_t now_ms)
{
    gt911_model_t *m = (gt911_model_t *)self->impl;
    if (!answers(m, now_ms)) {
        return HAL_ERR_NOT_FOUND;
    }
    for (size_t i = 0u; i < n; ++i) {
        d[i] = read_reg(m, m->pointer);
        m->pointer = (uint16_t)(m->pointer + 1u);
    }
    return HAL_OK;
}

void gt911_model_init(gt911_model_t *m)
{
    memset(m, 0, sizeof(*m));
    memcpy(m->product_id, "911", 4u);
    m->firmware = GT911_MODEL_DEFAULT_FIRMWARE;
    m->config[AT_VERSION] = GT911_MODEL_DEFAULT_CONFIG_VERSION;
    m->config[AT_X_MAX] = lo(GT911_MODEL_DEFAULT_X_MAX);
    m->config[AT_X_MAX + 1u] = hi(GT911_MODEL_DEFAULT_X_MAX);
    m->config[AT_Y_MAX] = lo(GT911_MODEL_DEFAULT_Y_MAX);
    m->config[AT_Y_MAX + 1u] = hi(GT911_MODEL_DEFAULT_Y_MAX);
    m->config[AT_TOUCH_NUMBER] = GT911_MODEL_DEFAULT_TOUCH_NUMBER;
    m->config[AT_MODULE_SWITCH1] = GT911_MODEL_DEFAULT_MODULE_SWITCH1;
    gt911_model_fix_checksum(m);
    m->model.write = model_write;
    m->model.read = model_read;
    m->model.impl = m;
}

i2c_model_t *gt911_model_port(gt911_model_t *m)
{
    return &m->model;
}

void gt911_model_set_interrupt(gt911_model_t *m, gt911_model_interrupt_fn fn, void *ctx)
{
    m->on_interrupt = fn;
    m->on_interrupt_ctx = ctx;
}

void gt911_model_int_drive_low(gt911_model_t *m, uint64_t now_ms)
{
    (void)now_ms;
    m->int_level = GT911_MODEL_INT_HOST_LOW;
}

void gt911_model_int_release(gt911_model_t *m, uint64_t now_ms)
{
    (void)now_ms;
    m->int_level = GT911_MODEL_INT_RELEASED;
}

void gt911_model_int_pulse_high(gt911_model_t *m, uint32_t us, uint64_t now_ms)
{
    m->stats.last_pulse_us = us;
    m->int_level = GT911_MODEL_INT_RELEASED;
    if (!m->asleep) {
        m->stats.pulses_while_awake++;
        return;
    }
    if (now_ms < m->slept_at_ms + GT911_MODEL_SLEEP_TO_WAKE_MS) {
        m->stats.early_wake_pulses++;  // ASSUMED: stays asleep
        return;
    }
    if (us < GT911_MODEL_WAKE_PULSE_MIN_US) {
        m->stats.short_wake_pulses++;  // ASSUMED: stays asleep
        return;
    }
    if (us > GT911_MODEL_WAKE_PULSE_MAX_US) {
        m->stats.long_wake_pulses++;
    }
    m->asleep = false;
    m->stats.wakes++;
    m->answers_from_ms = now_ms + (us + US_PER_MS - 1u) / US_PER_MS + m->answer_delay_ms;
}

void gt911_model_touch(gt911_model_t *m, const gt911_model_point_t *points, size_t count)
{
    if (count > GT911_MODEL_MAX_POINTS) {
        count = GT911_MODEL_MAX_POINTS;
    }
    if (count > 0u) {
        memcpy(m->touching, points, count * sizeof(points[0]));
    }
    m->touching_count = count;
    report(m);  // ASSUMED for count 0: the lift is a report of its own
}

void gt911_model_scan(gt911_model_t *m)
{
    if (m->touching_count > 0u) {
        report(m);
    }
}

void gt911_model_drop_reports(gt911_model_t *m, unsigned n)
{
    m->drop_next = n;
}

void gt911_model_spurious_interrupt(gt911_model_t *m)
{
    m->stats.interrupts++;
    if (m->on_interrupt != NULL) {
        m->on_interrupt(m->on_interrupt_ctx);
    }
}
