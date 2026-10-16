// SPDX-License-Identifier: Apache-2.0

#include "scd4x_source.h"

#include <stddef.h>
#include <string.h>

#include "scd4x_hal.h"
#include "scd4x_i2c.h"
#include "scd4x_limits.h"
#include "sensirion_common.h"
#include "sensirion_i2c.h"

#define LOG_TAG "scd4x"

// The conversions of datasheet 3.6.2: T = -45 + 175 * word / 65535 and
// RH = 100 * word / 65535. Done here from the raw words, in double, rather
// than with Sensirion's fixed-point scd4x_read_measurement(), whose
// 21875 / 8192 approximates 175000 / 65535 closely enough for display but
// puts the word for exactly 60 C at 59.998 -- and the range check is at the
// datasheet's limits.
#define TICKS_FULL_SCALE 65535.0
#define TEMPERATURE_OFFSET_C 45.0
#define TEMPERATURE_SPAN_C 175.0
#define HUMIDITY_SPAN_RH 100.0

#define PA_PER_HPA 100.0f

// get_serial_number: three words, 48 bits, big-endian (datasheet 3.10.2).
#define SERIAL_WORDS 3u
#define SERIAL_BYTES 6u
#define SERIAL_HEX_DIGITS 12u
#define BITS_PER_HEX_DIGIT 4u

static scd4x_source_t *self_of(sensor_source_t *port)
{
    return (scd4x_source_t *)port->impl;
}

static uint64_t now_ms(const scd4x_source_t *s)
{
    return s->cfg.clock->now_ms(s->cfg.clock);
}

static const char *status_name(hal_status_t status)
{
    switch (status) {
        case HAL_OK:
            return "ok";
        case HAL_ERR_TIMEOUT:
            return "timeout";
        case HAL_ERR_IO:
            return "io";
        case HAL_ERR_NOT_FOUND:
            return "not_found";
        case HAL_ERR_INVALID_ARG:
            return "invalid_arg";
        case HAL_ERR_NO_MEM:
            return "no_mem";
        case HAL_ERR_UNSUPPORTED:
            return "unsupported";
        case HAL_ERR_BUSY:
            return "busy";
        case HAL_ERR_NOT_READY:
            return "not_ready";
        default:
            return "?";
    }
}

static const char *mode_name(scd4x_mode_t mode)
{
    return mode == SCD4X_MODE_SINGLE_SHOT ? "single_shot" : "periodic";
}

const char *scd4x_variant_name(scd4x_variant_t variant)
{
    switch (variant) {
        case SCD4X_VARIANT_SCD40:
            return "SCD40";
        case SCD4X_VARIANT_SCD41:
            return "SCD41";
        case SCD4X_VARIANT_SCD43:
            return "SCD43";
        default:
            return "unknown";
    }
}

// What Sensirion's int16_t error code was, as the port saw it. Its code
// returns the HAL's own value unchanged, or CRC_ERROR when a word arrived
// with a bad checksum; scd4x_hal.c records the port's real answer.
static hal_status_t vendor_status(int16_t error)
{
    if (error == NO_ERROR) {
        return HAL_OK;
    }
    if (error == CRC_ERROR) {
        return HAL_ERR_IO;
    }
    const scd4x_hal_transfer_t last = scd4x_hal_last_transfer();
    return last.status != HAL_OK ? last.status : HAL_ERR_IO;
}

static void format_serial(uint64_t serial, char *out)
{
    static const char HEX[] = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (unsigned i = 0; i < SERIAL_HEX_DIGITS; i++) {
        const unsigned shift = BITS_PER_HEX_DIGIT * (SERIAL_HEX_DIGITS - 1u - i);
        out[2u + i] = HEX[(serial >> shift) & 0xFu];
    }
    out[2u + SERIAL_HEX_DIGITS] = '\0';
}

static scd4x_variant_t variant_of(scd4x_sensor_variant v)
{
    switch (v) {
        case SCD4X_SENSOR_VARIANT_SCD40:
            return SCD4X_VARIANT_SCD40;
        case SCD4X_SENSOR_VARIANT_SCD41:
            return SCD4X_VARIANT_SCD41;
        case SCD4X_SENSOR_VARIANT_SCD43:
            return SCD4X_VARIANT_SCD43;
        default:
            return SCD4X_VARIANT_UNKNOWN;
    }
}

static void log_failure(const scd4x_source_t *s, const char *msg, const char *step,
                        hal_status_t status)
{
    const log_field_t fields[] = {
        log_str("step", step),
        log_str("status", status_name(status)),
        log_u64("address", s->cfg.address),
    };
    port_log_write(s->cfg.log, LOG_LEVEL_WARN, LOG_TAG, msg, fields,
                   sizeof fields / sizeof fields[0]);
}

// measure_single_shot without its wait. Sensirion's scd4x_measure_single_shot()
// sleeps the whole 5000 ms inside the call; this sends the same command
// through the same framing (sensirion_i2c.h) and leaves the wait to
// ready_in_ms().
static hal_status_t send_without_wait(const scd4x_source_t *s, uint16_t command)
{
    uint8_t buffer[SENSIRION_COMMAND_SIZE];
    const uint16_t n = sensirion_i2c_add_command16_to_buffer(buffer, 0u, command);
    return vendor_status(sensirion_i2c_write_data(s->cfg.address, buffer, n));
}

static void clear_sample(sensor_sample_t *out)
{
    out->temperature_c = measurement_unavailable();
    out->humidity_rh = measurement_unavailable();
    out->pressure_hpa = measurement_unavailable();
    out->co2_ppm = measurement_unavailable();
    out->iaq = measurement_unavailable();
    out->voc_index = measurement_unavailable();
    out->gas_resistance_ohm = measurement_unavailable();
}

static measurement_f32_t judge(const scd4x_source_t *s, float value, bool in_range, uint64_t now)
{
    if (!in_range) {
        return measurement_status(VAL_ERROR, now, s->cfg.id);
    }
    if (now < s->warmup_until_ms) {
        return measurement_status(VAL_WARMUP, now, s->cfg.id);
    }
    return measurement_ok(value, now, s->cfg.id);
}

static void fill_sample(const scd4x_source_t *s, sensor_sample_t *out, uint16_t co2_word,
                        uint16_t t_word, uint16_t rh_word, uint64_t now)
{
    const float t = (float)(-TEMPERATURE_OFFSET_C + TEMPERATURE_SPAN_C * t_word / TICKS_FULL_SCALE);
    const float rh = (float)(HUMIDITY_SPAN_RH * rh_word / TICKS_FULL_SCALE);

    clear_sample(out);
    out->co2_ppm = judge(s, (float)co2_word,
                         co2_word >= SCD4X_CO2_MIN_PPM && co2_word <= SCD4X_CO2_MAX_PPM, now);
    out->temperature_c =
        judge(s, t, t >= SCD4X_TEMPERATURE_MIN_C && t <= SCD4X_TEMPERATURE_MAX_C, now);
    out->humidity_rh =
        judge(s, rh, rh >= SCD4X_HUMIDITY_MIN_RH && rh <= SCD4X_HUMIDITY_MAX_RH, now);
}

// The bus or the sensor failed: every capability VAL_ERROR, and the next
// attempt no sooner than the sensor's own interval -- a failing sensor is not
// polled faster than a working one.
static hal_status_t fail(scd4x_source_t *s, sensor_sample_t *out, hal_status_t status)
{
    const uint64_t now = now_ms(s);
    clear_sample(out);
    out->co2_ppm = measurement_status(VAL_ERROR, now, s->cfg.id);
    out->temperature_c = measurement_status(VAL_ERROR, now, s->cfg.id);
    out->humidity_rh = measurement_status(VAL_ERROR, now, s->cfg.id);
    s->next_read_ms = now + SCD4X_PERIODIC_INTERVAL_MS;
    if (s->cfg.mode == SCD4X_MODE_SINGLE_SHOT) {
        s->measuring = false;  // the shot is lost; the next start() takes another
    }
    if (!s->failing) {
        log_failure(s, "read failed", "read", status);  // once, not every interval
        s->failing = true;
    }
    return status;
}

// Ask again soon -- but never sooner than a reading is expected anyway, so
// a read() before ready_in_ms() said to does not shorten the wait.
static hal_status_t not_yet(scd4x_source_t *s)
{
    const uint64_t retry_ms = now_ms(s) + SCD4X_NOT_READY_RETRY_MS;
    if (retry_ms > s->next_read_ms) {
        s->next_read_ms = retry_ms;
    }
    return HAL_ERR_NOT_READY;
}

// A failed read_measurement that means "not yet", not "gone" (scd4x_source.h,
// "Busy, not absent").
static bool means_not_yet(const scd4x_source_t *s, int16_t error)
{
    if (error == CRC_ERROR) {
        return false;
    }
    const scd4x_hal_transfer_t last = scd4x_hal_last_transfer();
    if (last.status != HAL_ERR_NOT_FOUND) {
        return false;
    }
    if (last.was_read) {
        return true;  // the command was acknowledged; nothing is buffered yet
    }
    return s->cfg.mode == SCD4X_MODE_SINGLE_SHOT &&
           now_ms(s) < s->busy_until_ms + SCD4X_BUSY_GRACE_MS;
}

static hal_status_t src_init(sensor_source_t *self)
{
    scd4x_source_t *s = self_of(self);
    s->initialized = false;
    s->measuring = false;
    s->warmup_started = false;
    s->warmup_until_ms = 0u;
    s->next_read_ms = 0u;
    s->busy_until_ms = 0u;
    s->failing = false;
    s->found_measuring = false;
    s->serial = 0u;
    s->variant = SCD4X_VARIANT_UNKNOWN;
    format_serial(0u, s->serial_text);

    hal_status_t status = scd4x_hal_claim(s, s->cfg.bus, s->cfg.clock);
    if (status != HAL_OK) {
        log_failure(s, "sensor not usable", "claim", status);
        return status;
    }
    scd4x_init(s->cfg.address);

    // The supply may have risen only just now (scd4x_limits.h).
    s->cfg.clock->delay_ms(s->cfg.clock, SCD4X_POWER_UP_MS);

    // Stop first: a sensor left measuring by a reset that kept its supply
    // refuses everything else (scd4x_source.h, "Resets"). Asking for the
    // serial number just before shows which case this is; it changes
    // nothing, and the stop runs either way. Then reinit, so its settings
    // are what EEPROM holds whatever the last firmware changed.
    uint16_t words[SERIAL_WORDS] = {0u, 0u, 0u};
    const bool refused_before_stop = scd4x_get_serial_number(words, SERIAL_WORDS) != NO_ERROR;
    status = vendor_status(scd4x_stop_periodic_measurement());
    if (status != HAL_OK) {
        log_failure(s, "sensor not usable", "stop_periodic_measurement", status);
        return status;
    }
    status = vendor_status(scd4x_reinit());
    if (status != HAL_OK) {
        log_failure(s, "sensor not usable", "reinit", status);
        return status;
    }

    // Refused before the stop, answering after it: it was measuring. Refused
    // before and the stop failed too: it is not there, reported above.
    s->found_measuring = refused_before_stop;

    status = vendor_status(scd4x_get_serial_number(words, SERIAL_WORDS));
    if (status != HAL_OK) {
        log_failure(s, "sensor not usable", "get_serial_number", status);
        return status;
    }
    // Sensirion copies the serial's bytes into `words` as they came off the
    // wire, most significant first, whatever this CPU's byte order.
    const uint8_t *bytes = (const uint8_t *)words;
    for (unsigned i = 0; i < SERIAL_BYTES; i++) {
        s->serial = (s->serial << 8) | bytes[i];
    }
    format_serial(s->serial, s->serial_text);

    // get_sensor_variant arrived in datasheet version 1.6; firmware older
    // than that may refuse it, which leaves the variant unknown rather than
    // the sensor unusable. Any other failure is the bus.
    scd4x_sensor_variant variant = SCD4X_SENSOR_VARIANT_MASK;
    status = vendor_status(scd4x_get_sensor_variant(&variant));
    if (status == HAL_OK) {
        s->variant = variant_of(variant);
    } else if (status != HAL_ERR_IO) {
        log_failure(s, "sensor not usable", "get_sensor_variant", status);
        return status;
    }

    if (s->cfg.mode == SCD4X_MODE_SINGLE_SHOT && s->variant == SCD4X_VARIANT_SCD40) {
        log_failure(s, "single shot needs an SCD41 or SCD43", "mode", HAL_ERR_UNSUPPORTED);
        return HAL_ERR_UNSUPPORTED;
    }

    const log_field_t fields[] = {
        log_u64("address", s->cfg.address),
        log_str("serial", s->serial_text),
        log_str("variant", scd4x_variant_name(s->variant)),
        log_str("mode", mode_name(s->cfg.mode)),
        log_bool("found_measuring", s->found_measuring),
    };
    port_log_write(s->cfg.log, LOG_LEVEL_INFO, LOG_TAG, "sensor ready", fields,
                   sizeof fields / sizeof fields[0]);
    s->initialized = true;
    return HAL_OK;
}

static hal_status_t src_start(sensor_source_t *self)
{
    scd4x_source_t *s = self_of(self);
    if (!s->initialized) {
        return HAL_ERR_INVALID_ARG;
    }
    const uint64_t now = now_ms(s);

    if (s->cfg.mode == SCD4X_MODE_PERIODIC) {
        if (!s->measuring) {
            const hal_status_t status = vendor_status(scd4x_start_periodic_measurement());
            if (status != HAL_OK) {
                return status;
            }
            s->measuring = true;
            s->next_read_ms = now + SCD4X_PERIODIC_INTERVAL_MS;
        }
    } else {
        if (now < s->busy_until_ms) {
            return HAL_ERR_BUSY;  // the previous shot is still being measured
        }
        const hal_status_t status =
            send_without_wait(s, (uint16_t)SCD4X_MEASURE_SINGLE_SHOT_CMD_ID);
        if (status != HAL_OK) {
            return status;
        }
        s->measuring = true;
        s->busy_until_ms = now + SCD4X_SINGLE_SHOT_MS;
        s->next_read_ms = s->busy_until_ms;
    }

    if (!s->warmup_started) {
        s->warmup_started = true;
        s->warmup_until_ms = now + SCD4X_WARMUP_MS;
    }
    return HAL_OK;
}

static uint32_t src_ready_in_ms(const sensor_source_t *self)
{
    const scd4x_source_t *s = (const scd4x_source_t *)self->impl;
    if (!s->measuring) {
        return 0u;
    }
    const uint64_t now = now_ms(s);
    if (s->next_read_ms <= now) {
        return 0u;
    }
    const uint64_t wait = s->next_read_ms - now;
    return wait > UINT32_MAX ? UINT32_MAX : (uint32_t)wait;
}

static hal_status_t src_read(sensor_source_t *self, sensor_sample_t *out)
{
    scd4x_source_t *s = self_of(self);
    if (!s->initialized || !s->measuring) {
        return HAL_ERR_NOT_READY;
    }
    const bool single_shot = (s->cfg.mode == SCD4X_MODE_SINGLE_SHOT);
    if (single_shot && now_ms(s) < s->busy_until_ms) {
        return HAL_ERR_NOT_READY;  // measuring: the sensor must not be addressed
    }

    if (!single_shot) {
        bool ready = false;
        const hal_status_t status = vendor_status(scd4x_get_data_ready_status(&ready));
        if (status != HAL_OK) {
            return fail(s, out, status);
        }
        if (!ready) {
            return not_yet(s);
        }
    }

    uint16_t co2 = 0u;
    uint16_t t = 0u;
    uint16_t rh = 0u;
    const int16_t error = scd4x_read_measurement_raw(&co2, &t, &rh);
    if (error != NO_ERROR) {
        if (means_not_yet(s, error)) {
            return not_yet(s);
        }
        return fail(s, out, vendor_status(error));
    }

    const uint64_t now = now_ms(s);
    fill_sample(s, out, co2, t, rh, now);
    if (s->failing) {
        s->failing = false;
        port_log_write(s->cfg.log, LOG_LEVEL_INFO, LOG_TAG, "reading again", NULL, 0u);
    }
    if (single_shot) {
        s->measuring = false;  // collected; the next start() takes another
    } else {
        s->next_read_ms = now + SCD4X_PERIODIC_INTERVAL_MS;
    }
    return HAL_OK;
}

static hal_status_t src_suspend(sensor_source_t *self)
{
    scd4x_source_t *s = self_of(self);
    if (!s->initialized) {
        return HAL_OK;
    }
    if (s->cfg.mode == SCD4X_MODE_PERIODIC) {
        if (s->measuring) {
            const hal_status_t status = vendor_status(scd4x_stop_periodic_measurement());
            if (status != HAL_OK) {
                return status;
            }
            s->measuring = false;
        }
        return HAL_OK;
    }
    if (now_ms(s) < s->busy_until_ms) {
        return HAL_ERR_BUSY;  // a shot is being measured; ready_in_ms() says how long
    }
    s->measuring = false;
    return HAL_OK;
}

static void src_deinit(sensor_source_t *self)
{
    scd4x_source_t *s = self_of(self);
    s->initialized = false;
    s->measuring = false;
    scd4x_hal_release(s);
}

// The pressure, for the CO2 compensation; nothing else in `ambient` concerns
// this sensor. reinit() in init() puts the sensor back on its EEPROM's
// altitude setting, which is why the sensor manager hands this over again
// after every init() (docs/adr/0018-ambient-compensation-through-port-sensor.md).
static hal_status_t src_set_ambient(sensor_source_t *self, const sensor_ambient_t *ambient)
{
    scd4x_source_t *s = self_of(self);
    if (!s->initialized) {
        return HAL_ERR_NOT_READY;
    }
    if (ambient->pressure_hpa.status != VAL_OK) {
        return HAL_OK;
    }
    return scd4x_source_set_ambient_pressure(s, ambient->pressure_hpa.value);
}

hal_status_t scd4x_source_setup(scd4x_source_t *s, const scd4x_source_config_t *cfg)
{
    if (s == NULL || cfg == NULL || cfg->bus == NULL || cfg->clock == NULL) {
        return HAL_ERR_INVALID_ARG;
    }
    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    format_serial(0u, s->serial_text);

    s->port.name = LOG_TAG;
    s->port.id = cfg->id;
    s->port.caps = SENS_CAP_CO2 | SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY;
    s->port.init = src_init;
    s->port.start = src_start;
    s->port.ready_in_ms = src_ready_in_ms;
    s->port.read = src_read;
    s->port.suspend = src_suspend;
    s->port.deinit = src_deinit;
    s->port.set_ambient = src_set_ambient;
    s->port.impl = s;
    return HAL_OK;
}

hal_status_t scd4x_source_set_ambient_pressure(scd4x_source_t *s, float pressure_hpa)
{
    // Written so that NaN fails it too.
    if (!s->initialized || !(pressure_hpa >= SCD4X_AMBIENT_PRESSURE_MIN_HPA &&
                             pressure_hpa <= SCD4X_AMBIENT_PRESSURE_MAX_HPA)) {
        return HAL_ERR_INVALID_ARG;
    }
    if (s->cfg.mode == SCD4X_MODE_SINGLE_SHOT && now_ms(s) < s->busy_until_ms) {
        return HAL_ERR_BUSY;
    }
    const uint32_t pa = (uint32_t)(pressure_hpa * PA_PER_HPA + 0.5f);
    return vendor_status(scd4x_set_ambient_pressure(pa));
}
