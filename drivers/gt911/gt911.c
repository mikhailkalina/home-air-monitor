// SPDX-License-Identifier: Apache-2.0

#include "gt911.h"

#include <string.h>

#define LOG_TAG "gt911"

#define REG_ADDR_LEN 2u

#define CHECKSUM_AT (GT911_REG_CONFIG_CHECKSUM - GT911_REG_CONFIG_FIRST)

static void put_reg(uint8_t *dst, uint16_t reg)
{
    dst[0] = (uint8_t)(reg >> 8);
    dst[1] = (uint8_t)(reg & 0xFFu);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static hal_status_t read_regs(gt911_t *t, uint8_t address, uint16_t reg, uint8_t *out, size_t n)
{
    uint8_t w[REG_ADDR_LEN];
    put_reg(w, reg);
    return t->cfg.bus->write_read(t->cfg.bus, address, w, sizeof(w), out, n,
                                  GT911_TRANSFER_TIMEOUT_MS);
}

static hal_status_t write_reg(gt911_t *t, uint16_t reg, uint8_t value)
{
    uint8_t w[REG_ADDR_LEN + 1u];
    put_reg(w, reg);
    w[REG_ADDR_LEN] = value;
    return t->cfg.bus->write(t->cfg.bus, t->info.address, w, sizeof(w), GT911_TRANSFER_TIMEOUT_MS);
}

uint8_t gt911_config_checksum(const uint8_t *config)
{
    uint8_t sum = 0u;
    for (size_t i = 0u; i < CHECKSUM_AT; ++i) {
        sum = (uint8_t)(sum + config[i]);
    }
    return (uint8_t)(~sum + 1u);
}

const char *gt911_trigger_name(gt911_trigger_t trigger)
{
    switch (trigger) {
        case GT911_TRIGGER_RISING:
            return "rising";
        case GT911_TRIGGER_FALLING:
            return "falling";
        case GT911_TRIGGER_LOW_LEVEL:
            return "low";
        case GT911_TRIGGER_HIGH_LEVEL:
            return "high";
    }
    return "?";
}

hal_status_t gt911_setup(gt911_t *t, const gt911_config_t *cfg)
{
    memset(t, 0, sizeof(*t));
    if (cfg == NULL || cfg->bus == NULL || cfg->clock == NULL || cfg->address == 0u) {
        return HAL_ERR_INVALID_ARG;
    }
    t->cfg = *cfg;
    return HAL_OK;
}

// An address-only probe: no data, so a controller that does not answer
// costs one refused address and nothing else -- above all no error lines.
// ESP-IDF's I2C driver logs three for every failed data transfer, from the
// caller's stack; a second of them on app_main's 3.5 KB overflowed it on the
// board (2026-10-20, docs/hardware/board_notes.md, "The touch panel on the
// board"). A probe that is refused it does not log at all.
static hal_status_t probe(gt911_t *t, uint8_t address)
{
    return t->cfg.bus->write(t->cfg.bus, address, NULL, 0u, GT911_TRANSFER_TIMEOUT_MS);
}

// Which of `candidates` answers, probed in turn -- each attempt asks every
// one -- until one does or GT911_ANSWER_TIMEOUT_MS has passed. Counts the
// attempts. HAL_ERR_NOT_FOUND when none answered; another status as soon as
// a probe fails otherwise, which is the bus, not the address.
static hal_status_t find_address(gt911_t *t, const uint8_t *candidates, size_t n, uint8_t *found)
{
    const port_clock_t *clock = t->cfg.clock;
    const uint64_t give_up_at = clock->now_ms(clock) + GT911_ANSWER_TIMEOUT_MS;
    for (;;) {
        t->answer_attempts++;
        for (size_t i = 0u; i < n; ++i) {
            if (candidates[i] == 0u) {
                continue;
            }
            const hal_status_t st = probe(t, candidates[i]);
            if (st == HAL_OK) {
                *found = candidates[i];
                return HAL_OK;
            }
            if (st != HAL_ERR_NOT_FOUND) {
                return st;
            }
        }
        if (clock->now_ms(clock) >= give_up_at) {
            return HAL_ERR_NOT_FOUND;
        }
        clock->delay_ms(clock, GT911_ANSWER_RETRY_MS);
    }
}

static void decode_product_info(gt911_info_t *info, const uint8_t *raw)
{
    memcpy(info->product_id, raw, GT911_PRODUCT_ID_LEN);
    info->product_id[GT911_PRODUCT_ID_LEN] = '\0';
    // Printable ASCII only, so a log line or the console can show it as is.
    for (size_t i = 0u; i < GT911_PRODUCT_ID_LEN; ++i) {
        const char c = info->product_id[i];
        if (c != '\0' && (c < ' ' || c > '~')) {
            info->product_id[i] = '?';
        }
    }
    info->firmware_version = le16(&raw[4]);
    info->x_resolution = le16(&raw[6]);
    info->y_resolution = le16(&raw[8]);
    info->vendor_id = raw[10];
}

static void decode_config(gt911_info_t *info, const uint8_t *config)
{
    info->config_version = config[GT911_CONFIG_VERSION];
    info->x_max = le16(&config[GT911_CONFIG_X_MAX]);
    info->y_max = le16(&config[GT911_CONFIG_Y_MAX]);
    info->touch_number =
        (uint8_t)(config[GT911_CONFIG_TOUCH_NUMBER] & GT911_CONFIG_TOUCH_NUMBER_MASK);
    info->module_switch1 = config[GT911_CONFIG_MODULE_SWITCH1];
    info->trigger = (gt911_trigger_t)(info->module_switch1 & GT911_CONFIG_TRIGGER_MASK);
    info->config_checksum = config[CHECKSUM_AT];
    info->config_checksum_ok = (gt911_config_checksum(config) == config[CHECKSUM_AT]);
}

// "911" and NUL padding. A GT911's ID is three digits; anything else at
// this register is not the part this map belongs to.
static bool is_gt911(const gt911_info_t *info)
{
    return strcmp(info->product_id, GT911_PRODUCT_ID) == 0;
}

static void log_found(const gt911_t *t)
{
    const gt911_info_t *i = &t->info;
    const log_field_t fields[] = {
        log_u64("address", i->address),
        log_str("product_id", i->product_id),
        log_u64("firmware", i->firmware_version),
        log_u64("attempts", t->answer_attempts),
        log_str("trigger", gt911_trigger_name(i->trigger)),
        log_bool("config_checksum_ok", i->config_checksum_ok),
    };
    port_log_write(t->cfg.log, LOG_LEVEL_INFO, LOG_TAG, "found", fields,
                   sizeof(fields) / sizeof(fields[0]));
    const log_field_t config_fields[] = {
        log_u64("config_version", i->config_version),
        log_u64("x_max", i->x_max),
        log_u64("y_max", i->y_max),
        log_u64("touch_number", i->touch_number),
        log_u64("module_switch1", i->module_switch1),
        log_u64("resolution_x", i->x_resolution),
    };
    port_log_write(t->cfg.log, LOG_LEVEL_INFO, LOG_TAG, "configuration (read, never written)",
                   config_fields, sizeof(config_fields) / sizeof(config_fields[0]));
}

static hal_status_t pulse_awake(gt911_t *t)
{
    const gt911_int_line_t *line = t->cfg.int_line;
    const port_clock_t *clock = t->cfg.clock;
    if (t->slept_at_known) {
        const uint64_t now = clock->now_ms(clock);
        const uint64_t earliest = t->slept_at_ms + GT911_SLEEP_TO_WAKE_MS + 1u;
        if (now < earliest) {
            clock->delay_ms(clock, (uint32_t)(earliest - now));
        }
    }
    line->pulse_high(line->ctx, GT911_WAKE_PULSE_US);
    clock->delay_ms(clock, GT911_WAKE_SETTLE_MS);
    t->asleep = false;
    t->slept_at_known = false;
    return HAL_OK;
}

hal_status_t gt911_init(gt911_t *t)
{
    t->found = false;
    t->answer_attempts = 0u;
    memset(&t->info, 0, sizeof(t->info));

    // Asleep or not, nothing else wakes it: a reset of the ESP32 does not
    // reach it (gt911.h). A pulse on a controller that is awake does
    // nothing -- INT is its output then, and the pulse lasts 3 ms.
    if (t->cfg.int_line != NULL) {
        (void)pulse_awake(t);
    }

    // Found by probing, both addresses in turn: the latch can change with a
    // power glitch the ESP32 does not see (seen 2026-10-20), so neither is
    // more likely than the other at any given boot.
    uint8_t raw[GT911_PRODUCT_INFO_LEN];
    const uint8_t candidates[2] = {t->cfg.address, t->cfg.alt_address};
    uint8_t address = 0u;
    hal_status_t st = find_address(t, candidates, sizeof(candidates), &address);
    if (st == HAL_OK) {
        t->info.address = address;
        st = read_regs(t, address, GT911_REG_PRODUCT_ID, raw, GT911_PRODUCT_INFO_LEN);
    }
    if (st != HAL_OK) {
        const log_field_t fields[] = {
            log_u64("address", t->cfg.address),
            log_u64("alt_address", t->cfg.alt_address),
            log_u64("attempts", t->answer_attempts),
            log_i64("status", (int64_t)st),
        };
        port_log_write(t->cfg.log, LOG_LEVEL_ERROR, LOG_TAG, "not found", fields,
                       sizeof(fields) / sizeof(fields[0]));
        return st;
    }

    decode_product_info(&t->info, raw);
    if (!is_gt911(&t->info)) {
        const log_field_t fields[] = {
            log_u64("address", t->info.address),
            log_str("product_id", t->info.product_id),
        };
        port_log_write(t->cfg.log, LOG_LEVEL_ERROR, LOG_TAG,
                       "answers, but is not a GT911: its register map may differ", fields,
                       sizeof(fields) / sizeof(fields[0]));
        return HAL_ERR_UNSUPPORTED;
    }

    uint8_t config[GT911_CONFIG_LEN];
    st = read_regs(t, t->info.address, GT911_REG_CONFIG_FIRST, config, sizeof(config));
    if (st != HAL_OK) {
        return st;
    }
    decode_config(&t->info, config);

    // A report left from before this init -- or from before the ESP32's
    // reset -- describes a touch nobody is waiting for any more.
    st = write_reg(t, GT911_REG_STATUS, 0u);
    if (st != HAL_OK) {
        return st;
    }
    t->found = true;
    log_found(t);
    return HAL_OK;
}

hal_status_t gt911_read_frame(gt911_t *t, gt911_frame_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!t->found || t->asleep) {
        return HAL_ERR_NOT_READY;
    }

    uint8_t status = 0u;
    hal_status_t st = read_regs(t, t->info.address, GT911_REG_STATUS, &status, 1u);
    if (st != HAL_OK) {
        return st;
    }
    out->status = status;
    if ((status & GT911_STATUS_BUFFER_READY) == 0u) {
        return HAL_OK;  // nothing ready: an early interrupt, or a poll
    }

    const uint8_t count = (uint8_t)(status & GT911_STATUS_COUNT_MASK);
    st = HAL_OK;
    if (count > GT911_MAX_POINTS) {
        st = HAL_ERR_IO;  // a status no GT911 reports: read nothing, clear it
    } else if (count > 0u) {
        uint8_t raw[GT911_MAX_POINTS * GT911_POINT_RECORD_LEN];
        st = read_regs(t, t->info.address, GT911_REG_POINTS, raw,
                       (size_t)count * GT911_POINT_RECORD_LEN);
        if (st == HAL_OK) {
            for (uint8_t i = 0u; i < count; ++i) {
                const uint8_t *r = &raw[(size_t)i * GT911_POINT_RECORD_LEN];
                out->points[i].track_id = r[0];
                out->points[i].x = le16(&r[1]);
                out->points[i].y = le16(&r[3]);
                out->points[i].size = le16(&r[5]);
            }
            out->count = count;
        }
    }

    // Cleared whatever was read: the controller holds its next report back
    // until this write.
    const hal_status_t cleared = write_reg(t, GT911_REG_STATUS, 0u);
    if (st != HAL_OK) {
        return st;
    }
    if (cleared != HAL_OK) {
        return cleared;
    }
    out->ready = true;
    return HAL_OK;
}

hal_status_t gt911_sleep(gt911_t *t)
{
    if (t->cfg.int_line == NULL) {
        return HAL_ERR_UNSUPPORTED;
    }
    if (!t->found) {
        return HAL_ERR_NOT_READY;
    }
    if (t->asleep) {
        return HAL_OK;
    }
    const gt911_int_line_t *line = t->cfg.int_line;
    line->drive_low(line->ctx);
    const hal_status_t st = write_reg(t, GT911_REG_COMMAND, GT911_CMD_SCREEN_OFF);
    if (st != HAL_OK) {
        line->release(line->ctx);
        return st;
    }
    t->asleep = true;
    t->slept_at_known = true;
    t->slept_at_ms = t->cfg.clock->now_ms(t->cfg.clock);
    port_log_write(t->cfg.log, LOG_LEVEL_INFO, LOG_TAG, "asleep: INT held low until woken", NULL,
                   0u);
    return HAL_OK;
}

hal_status_t gt911_wake(gt911_t *t)
{
    if (t->cfg.int_line == NULL) {
        return HAL_ERR_UNSUPPORTED;
    }
    if (!t->found) {
        return HAL_ERR_NOT_READY;
    }
    (void)pulse_awake(t);
    t->answer_attempts = 0u;
    const uint8_t candidates[1] = {t->info.address};
    uint8_t address = 0u;
    hal_status_t st = find_address(t, candidates, sizeof(candidates), &address);
    if (st == HAL_OK) {
        st = write_reg(t, GT911_REG_STATUS, 0u);
    }
    const log_field_t fields[] = {
        log_u64("attempts", t->answer_attempts),
        log_i64("status", (int64_t)st),
    };
    port_log_write(t->cfg.log, (st == HAL_OK) ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR, LOG_TAG, "woken",
                   fields, sizeof(fields) / sizeof(fields[0]));
    return st;
}

const gt911_info_t *gt911_info(const gt911_t *t)
{
    return &t->info;
}
