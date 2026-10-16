// SPDX-License-Identifier: Apache-2.0

#include "bme68x_source.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "bme68x_common.h"
#include "bme68x_limits.h"

// Bosch's float output (bme68x_defs.h's default) is what the conversions
// below are written for; the integer build reports hundredths of a degree
// and thousandths of a percent in the same fields.
#ifndef BME68X_USE_FPU
#error "bme68x_source.c expects Bosch's floating-point output (BME68X_USE_FPU)"
#endif

#define LOG_TAG "bme68x"

#define PA_PER_HPA 100.0f
#define US_PER_MS 1000u

static bme68x_source_t *self_of(sensor_source_t *port)
{
    return (bme68x_source_t *)port->impl;
}

static uint64_t now_ms(const bme68x_source_t *s)
{
    return s->cfg.clock->now_ms(s->cfg.clock);
}

static void log_failure(const bme68x_source_t *s, const char *msg, const char *step,
                        hal_status_t status)
{
    const log_field_t fields[] = {
        log_str("step", step),
        log_str("status", bme68x_status_name(status)),
        log_u64("address", s->cfg.address),
    };
    port_log_write(s->cfg.log, LOG_LEVEL_WARN, LOG_TAG, msg, fields,
                   sizeof fields / sizeof fields[0]);
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

static bool within(float value, float min, float max)
{
    return value >= min && value <= max;  // false for NaN, too
}

static float saturation_ratio(float t_c)
{
    return expf(HAC_BME68X_MAGNUS_B * t_c / (HAC_BME68X_MAGNUS_C_C + t_c));
}

float bme68x_relative_humidity_at(float rh, float from_c, float to_c)
{
    const float moved = rh * saturation_ratio(from_c) / saturation_ratio(to_c);
    if (moved > HAC_BME68X_HUMIDITY_MAX_RH) {
        return HAC_BME68X_HUMIDITY_MAX_RH;
    }
    if (moved < HAC_BME68X_HUMIDITY_MIN_RH) {
        return HAC_BME68X_HUMIDITY_MIN_RH;
    }
    return moved;
}

// Gas: only a value the sensor marked both valid and heater-stable is a
// reading (datasheet 3.4). Logged once per streak of invalid ones, with the
// two bits, since which one is missing tells a short heater profile from a
// gas measurement that did not run.
static measurement_f32_t judge_gas(bme68x_source_t *s, const struct bme68x_data *d, uint64_t now)
{
    const bool valid = (d->status & BME68X_GASM_VALID_MSK) != 0u;
    const bool stable = (d->status & BME68X_HEAT_STAB_MSK) != 0u;
    if (!valid || !stable) {
        if (!s->gas_failing) {
            const log_field_t fields[] = {
                log_bool("gas_valid", valid),
                log_bool("heat_stab", stable),
            };
            port_log_write(s->cfg.log, LOG_LEVEL_WARN, LOG_TAG, "gas not measured", fields,
                           sizeof fields / sizeof fields[0]);
            s->gas_failing = true;
        }
        return measurement_status(VAL_ERROR, now, s->cfg.id);
    }
    s->gas_failing = false;
    if (now < s->burn_in_until_ms) {
        return measurement_status(VAL_WARMUP, now, s->cfg.id);
    }
    return measurement_ok(d->gas_resistance, now, s->cfg.id);
}

static void fill_sample(bme68x_source_t *s, sensor_sample_t *out, const struct bme68x_data *d,
                        uint64_t now)
{
    clear_sample(out);

    // Humidity and pressure are compensated with the temperature (t_fine,
    // datasheet 3.3.2-3.3.3), so a temperature outside the operating range
    // takes them with it.
    const float t_die = d->temperature;
    const float rh_die = d->humidity;
    const float p_hpa = d->pressure / PA_PER_HPA;
    if (!within(t_die, HAC_BME68X_TEMPERATURE_MIN_C, HAC_BME68X_TEMPERATURE_MAX_C)) {
        out->temperature_c = measurement_status(VAL_ERROR, now, s->cfg.id);
        out->humidity_rh = measurement_status(VAL_ERROR, now, s->cfg.id);
        out->pressure_hpa = measurement_status(VAL_ERROR, now, s->cfg.id);
    } else {
        const float t_air = t_die - HAC_BME68X_SELF_HEATING_C;
        out->temperature_c = measurement_ok(t_air, now, s->cfg.id);
        out->humidity_rh =
            within(rh_die, HAC_BME68X_HUMIDITY_MIN_RH, HAC_BME68X_HUMIDITY_MAX_RH)
                ? measurement_ok(bme68x_relative_humidity_at(rh_die, t_die, t_air), now, s->cfg.id)
                : measurement_status(VAL_ERROR, now, s->cfg.id);
        out->pressure_hpa = within(p_hpa, HAC_BME68X_PRESSURE_MIN_HPA, HAC_BME68X_PRESSURE_MAX_HPA)
                                ? measurement_ok(p_hpa, now, s->cfg.id)
                                : measurement_status(VAL_ERROR, now, s->cfg.id);
    }
    out->gas_resistance_ohm = judge_gas(s, d, now);
}

// The bus or the sensor failed: every capability VAL_ERROR, and the
// measurement abandoned -- the next start() takes another.
static hal_status_t fail(bme68x_source_t *s, sensor_sample_t *out, hal_status_t status,
                         const char *step)
{
    const uint64_t now = now_ms(s);
    clear_sample(out);
    out->temperature_c = measurement_status(VAL_ERROR, now, s->cfg.id);
    out->humidity_rh = measurement_status(VAL_ERROR, now, s->cfg.id);
    out->pressure_hpa = measurement_status(VAL_ERROR, now, s->cfg.id);
    out->gas_resistance_ohm = measurement_status(VAL_ERROR, now, s->cfg.id);
    s->measuring = false;
    if (!s->failing) {
        log_failure(s, "read failed", step, status);  // once, not every measurement
        s->failing = true;
    }
    return status;
}

static hal_status_t src_init(sensor_source_t *self)
{
    bme68x_source_t *s = self_of(self);
    s->initialized = false;
    s->measuring = false;
    s->measurement_ms = 0u;
    s->ready_at_ms = 0u;
    s->next_read_ms = 0u;
    s->burn_in_started = false;
    s->burn_in_until_ms = 0u;
    s->failing = false;
    s->gas_failing = false;
    s->chip_id = 0u;
    s->variant_id = 0u;
    bme68x_format_id(0u, s->chip_id_text);
    bme68x_format_id(0u, s->variant_id_text);

    // Start-up time, soft reset, ids, calibration, asleep (bme68x_common.h).
    // The reset puts a sensor a software reset left configured, or
    // measuring, back to its power-on state (bme68x_source.h, "Resets").
    s->hal.bus = s->cfg.bus;
    s->hal.clock = s->cfg.clock;
    s->hal.address = s->cfg.address;
    const char *step = "init";
    hal_status_t status = bme68x_bring_up(&s->dev, &s->hal, &step);
    s->chip_id = s->dev.chip_id;
    bme68x_format_id(s->chip_id, s->chip_id_text);
    if (status == HAL_ERR_UNSUPPORTED) {
        const log_field_t fields[] = {
            log_str("chip_id", s->chip_id_text),
            log_u64("address", s->cfg.address),
        };
        port_log_write(s->cfg.log, LOG_LEVEL_WARN, LOG_TAG, "not a BME68x", fields,
                       sizeof fields / sizeof fields[0]);
        return status;
    }
    if (status != HAL_OK) {
        log_failure(s, "sensor not usable", step, status);
        return status;
    }
    s->variant_id = (uint8_t)s->dev.variant_id;
    bme68x_format_id(s->variant_id, s->variant_id_text);

    s->conf.os_temp = HAC_BME68X_OS_TEMPERATURE;
    s->conf.os_pres = HAC_BME68X_OS_PRESSURE;
    s->conf.os_hum = HAC_BME68X_OS_HUMIDITY;
    s->conf.filter = HAC_BME68X_FILTER;
    s->conf.odr = BME68X_ODR_NONE;
    status = bme68x_vendor_status(&s->dev, bme68x_set_conf(&s->conf, &s->dev));
    if (status != HAL_OK) {
        log_failure(s, "sensor not usable", "set_conf", status);
        return status;
    }

    memset(&s->heater, 0, sizeof s->heater);
    s->heater.enable = BME68X_ENABLE;
    s->heater.heatr_temp = HAC_BME68X_HEATER_TEMPERATURE_C;
    s->heater.heatr_dur = HAC_BME68X_HEATER_DURATION_MS;
    status = bme68x_vendor_status(&s->dev,
                                  bme68x_set_heatr_conf(BME68X_FORCED_MODE, &s->heater, &s->dev));
    if (status != HAL_OK) {
        log_failure(s, "sensor not usable", "set_heatr_conf", status);
        return status;
    }

    // Bosch's estimate of the conversions, plus the heater's time; the same
    // sum its forced-mode example waits.
    const uint32_t us = bme68x_get_meas_dur(BME68X_FORCED_MODE, &s->conf, &s->dev) +
                        (uint32_t)HAC_BME68X_HEATER_DURATION_MS * US_PER_MS;
    s->measurement_ms = us / US_PER_MS + ((us % US_PER_MS) != 0u ? 1u : 0u);

    const log_field_t fields[] = {
        log_u64("address", s->cfg.address),
        log_str("chip_id", s->chip_id_text),
        log_str("variant", bme68x_variant_name(s->variant_id)),
        log_str("variant_id", s->variant_id_text),
        log_u64("measurement_ms", s->measurement_ms),
    };
    port_log_write(s->cfg.log, LOG_LEVEL_INFO, LOG_TAG, "sensor ready", fields,
                   sizeof fields / sizeof fields[0]);
    if (!s->calibration_logged) {
        bme68x_log_calibration(s->cfg.log, LOG_TAG, &s->dev.calib);
        s->calibration_logged = true;
    }
    s->initialized = true;
    return HAL_OK;
}

static hal_status_t src_start(sensor_source_t *self)
{
    bme68x_source_t *s = self_of(self);
    if (!s->initialized) {
        return HAL_ERR_INVALID_ARG;
    }
    const uint64_t now = now_ms(s);
    if (s->measuring && now < s->ready_at_ms) {
        return HAL_ERR_BUSY;  // the previous measurement is still running
    }

    // Asleep first: Bosch's bme68x_set_op_mode() would otherwise wait for it
    // with no bound (bme68x_common.h). HAL_ERR_BUSY: still measuring, past
    // Bosch's estimate.
    hal_status_t status = bme68x_check_asleep(&s->dev);
    if (status != HAL_OK) {
        return status;
    }
    status = bme68x_vendor_status(&s->dev, bme68x_set_op_mode(BME68X_FORCED_MODE, &s->dev));
    if (status != HAL_OK) {
        return status;
    }

    s->measuring = true;
    s->ready_at_ms = now + s->measurement_ms;
    s->next_read_ms = s->ready_at_ms;
    if (!s->burn_in_started) {
        s->burn_in_started = true;
        s->burn_in_until_ms = now + HAC_BME68X_GAS_BURN_IN_MS;
    }
    return HAL_OK;
}

static uint32_t src_ready_in_ms(const sensor_source_t *self)
{
    const bme68x_source_t *s = (const bme68x_source_t *)self->impl;
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
    bme68x_source_t *s = self_of(self);
    if (!s->initialized || !s->measuring) {
        return HAL_ERR_NOT_READY;
    }
    if (now_ms(s) < s->ready_at_ms) {
        return HAL_ERR_NOT_READY;  // not done by Bosch's estimate: the bus is left alone
    }

    // On no new data, Bosch's call has already polled five times, 10 ms
    // apart, before it says so.
    struct bme68x_data data;
    memset(&data, 0, sizeof data);
    uint8_t fields = 0u;
    hal_status_t status =
        bme68x_vendor_status(&s->dev, bme68x_get_data(BME68X_FORCED_MODE, &data, &fields, &s->dev));
    if (status == HAL_OK && fields == 0u) {
        status = HAL_ERR_NOT_READY;
    }
    if (status == HAL_ERR_NOT_READY) {
        const uint64_t now = now_ms(s);
        if (now >= s->ready_at_ms + HAC_BME68X_MEASUREMENT_GRACE_MS) {
            return fail(s, out, HAL_ERR_TIMEOUT, "measurement_overdue");
        }
        s->next_read_ms = now + HAC_BME68X_NOT_READY_RETRY_MS;
        return HAL_ERR_NOT_READY;
    }
    if (status != HAL_OK) {
        return fail(s, out, status, "get_data");
    }

    fill_sample(s, out, &data, now_ms(s));
    s->measuring = false;  // collected; the next start() takes another
    if (s->failing) {
        s->failing = false;
        port_log_write(s->cfg.log, LOG_LEVEL_INFO, LOG_TAG, "reading again", NULL, 0u);
    }
    return HAL_OK;
}

static hal_status_t src_suspend(sensor_source_t *self)
{
    bme68x_source_t *s = self_of(self);
    if (!s->initialized) {
        return HAL_OK;
    }
    if (s->measuring && now_ms(s) < s->ready_at_ms) {
        return HAL_ERR_BUSY;  // a measurement is running; ready_in_ms() says how long
    }
    // Between forced measurements the sensor sleeps by itself; a reading not
    // collected is dropped.
    s->measuring = false;
    return HAL_OK;
}

static void src_deinit(sensor_source_t *self)
{
    bme68x_source_t *s = self_of(self);
    s->initialized = false;
    s->measuring = false;
}

hal_status_t bme68x_source_setup(bme68x_source_t *s, const bme68x_source_config_t *cfg)
{
    if (s == NULL || cfg == NULL || cfg->bus == NULL || cfg->clock == NULL) {
        return HAL_ERR_INVALID_ARG;
    }
    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    bme68x_format_id(0u, s->chip_id_text);
    bme68x_format_id(0u, s->variant_id_text);

    s->port.name = LOG_TAG;
    s->port.id = cfg->id;
    s->port.caps = SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY | SENS_CAP_PRESSURE | SENS_CAP_GAS_RES;
    s->port.init = src_init;
    s->port.start = src_start;
    s->port.ready_in_ms = src_ready_in_ms;
    s->port.read = src_read;
    s->port.suspend = src_suspend;
    s->port.deinit = src_deinit;
    s->port.impl = s;
    return HAL_OK;
}
