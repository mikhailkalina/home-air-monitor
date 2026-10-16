// SPDX-License-Identifier: Apache-2.0

#include "bme68x_bsec_source.h"

#include <stddef.h>
#include <string.h>

#include "bme68x_bsec_status.h"
#include "bme68x_limits.h"

// Bosch's BSEC2 headers and the configuration, from the archive outside this
// repository (cmake/bsec.cmake).
#include "bsec_datatypes.h"
#include "bsec_iaq.h"
#include "bsec_interface_multi.h"

// Bosch's float output (bme68x_defs.h's default), as in bme68x_source.c: BSEC
// takes degrees, percent, pascals and ohms.
#ifndef BME68X_USE_FPU
#error "bme68x_bsec_source.c expects Bosch's floating-point output (BME68X_USE_FPU)"
#endif

#ifndef HAC_BSEC_CONFIG_NAME
#error "HAC_BSEC_CONFIG_NAME must be defined by the build (cmake/bsec.cmake)"
#endif

_Static_assert(HAC_BSEC_STATE_BYTES == BSEC_MAX_STATE_BLOB_SIZE,
               "HAC_BSEC_STATE_BYTES must be the linked BSEC's BSEC_MAX_STATE_BLOB_SIZE");

#define LOG_TAG "bme68x_bsec"

#define NS_PER_MS 1000000
#define US_PER_MS 1000u
#define PA_PER_HPA 100.0f

// What BSEC is asked for, all at the low-power rate: what the screen and the
// log show, and the two statuses the gas reading's is judged by. Not
// BSEC_OUTPUT_CO2_EQUIVALENT: an estimate from VOCs that must never be shown
// or published as CO2 (docs/requirements.md 4.1) is safest never computed.
// Not BSEC_OUTPUT_STATIC_IAQ or BSEC_OUTPUT_BREATH_VOC_EQUIVALENT: nothing
// here shows them. Raw pressure and gas are subscribed to although the sensor
// measures them anyway: an output nobody asks for is a measurement BSEC need
// not request, and the pressure would then not be converted at all.
static const uint8_t SUBSCRIBED[] = {
    BSEC_OUTPUT_IAQ,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,
    BSEC_OUTPUT_RAW_PRESSURE,
    BSEC_OUTPUT_RAW_GAS,
    BSEC_OUTPUT_STABILIZATION_STATUS,
    BSEC_OUTPUT_RUN_IN_STATUS,
};
#define SUBSCRIBED_COUNT (sizeof SUBSCRIBED / sizeof SUBSCRIBED[0])

// The scratch space BSEC parses its configuration and state blobs in. Only
// init() uses it, and init() is never re-entered: one task drives every
// source. Static rather than on the stack: 4 KB is half the event loop's.
static uint8_t s_work[BSEC_MAX_WORKBUFFER_SIZE];

static bme68x_bsec_source_t *self_of(sensor_source_t *port)
{
    return (bme68x_bsec_source_t *)port->impl;
}

static uint64_t now_ms(const bme68x_bsec_source_t *s)
{
    return s->cfg.clock->now_ms(s->cfg.clock);
}

// BSEC counts in nanoseconds; port_clock in milliseconds since boot, which
// is what both are measured from.
static int64_t ns_of(uint64_t ms)
{
    return (int64_t)ms * NS_PER_MS;
}

// Rounded up: a call at the millisecond before BSEC's time would be early.
static uint64_t ms_of(int64_t ns)
{
    if (ns <= 0) {
        return 0u;
    }
    return ((uint64_t)ns + (uint64_t)NS_PER_MS - 1u) / (uint64_t)NS_PER_MS;
}

static bool within(float value, float min, float max)
{
    return value >= min && value <= max;  // false for NaN, too
}

// Whether BSEC wants `input` from this measurement: bit (id - 1) of
// bsec_bme_settings_t::process_data (bsec_datatypes.h, BSEC_PROCESS_*).
static bool wants(uint32_t process_data, uint8_t input)
{
    return ((process_data >> (input - 1u)) & 1u) != 0u;
}

static const char *bsec_result_name(int result)
{
    switch (result) {
        case BSEC_OK:
            return "ok";
        case BSEC_W_SC_CALL_TIMING_VIOLATION:
            return "call_timing_violation";
        case BSEC_W_DOSTEPS_TSINTRADIFFOUTOFRANGE:
            return "timestamps_out_of_order";
        case BSEC_W_SU_SAMPLERATEMISMATCH:
            return "sample_rate_mismatch";
        case BSEC_E_DOSTEPS_VALUELIMITS:
            return "value_limits";
        case BSEC_E_CONFIG_VERSIONMISMATCH:
            return "config_version_mismatch";
        case BSEC_E_CONFIG_CRCMISMATCH:
            return "config_crc_mismatch";
        case BSEC_E_CONFIG_FEATUREMISMATCH:
            return "config_feature_mismatch";
        default:
            return "other";
    }
}

static void log_bsec(const bme68x_bsec_source_t *s, log_level_t level, const char *msg,
                     const char *step, int result)
{
    const log_field_t fields[] = {
        log_str("step", step),
        log_i64("bsec_status", result),
        log_str("meaning", bsec_result_name(result)),
    };
    port_log_write(s->cfg.log, level, LOG_TAG, msg, fields, sizeof fields / sizeof fields[0]);
}

static void log_failure(const bme68x_bsec_source_t *s, const char *msg, const char *step,
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

// "2.6.1.0", without stdio (drivers/ may not include it).
static size_t append_u8(char *out, size_t at, uint8_t v)
{
    char digits[3];
    size_t n = 0u;
    do {
        digits[n++] = (char)('0' + v % 10u);
        v = (uint8_t)(v / 10u);
    } while (v != 0u);
    while (n > 0u) {
        out[at++] = digits[--n];
    }
    return at;
}

static void format_version(const bsec_version_t *v, char out[BME68X_BSEC_VERSION_TEXT_LEN])
{
    size_t at = append_u8(out, 0u, v->major);
    out[at++] = '.';
    at = append_u8(out, at, v->minor);
    out[at++] = '.';
    at = append_u8(out, at, v->major_bugfix);
    out[at++] = '.';
    at = append_u8(out, at, v->minor_bugfix);
    out[at] = '\0';
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

static void every_field_failed(const bme68x_bsec_source_t *s, sensor_sample_t *out, uint64_t now)
{
    clear_sample(out);
    out->temperature_c = measurement_status(VAL_ERROR, now, s->cfg.id);
    out->humidity_rh = measurement_status(VAL_ERROR, now, s->cfg.id);
    out->pressure_hpa = measurement_status(VAL_ERROR, now, s->cfg.id);
    out->iaq = measurement_status(VAL_ERROR, now, s->cfg.id);
    out->gas_resistance_ohm = measurement_status(VAL_ERROR, now, s->cfg.id);
}

// The bus, the sensor or BSEC failed: every capability VAL_ERROR, and the
// measurement abandoned -- BSEC's next call starts another.
static hal_status_t fail(bme68x_bsec_source_t *s, sensor_sample_t *out, hal_status_t status,
                         const char *step)
{
    every_field_failed(s, out, now_ms(s));
    s->measuring = false;
    if (!s->failing) {
        log_failure(s, "read failed", step, status);  // once, not every measurement
        s->failing = true;
    }
    return status;
}

// --- BSEC's instance ---------------------------------------------------------

// BSEC reset into this source's instance, configured and subscribed; what it
// had learnt before, if it ran, given back.
static hal_status_t start_bsec(bme68x_bsec_source_t *s)
{
    const size_t needed = bsec_get_instance_size_m();
    if (needed > sizeof s->instance) {
        const log_field_t fields[] = {
            log_u64("needed_bytes", needed),
            log_u64("reserved_bytes", sizeof s->instance),
        };
        port_log_write(s->cfg.log, LOG_LEVEL_ERROR, LOG_TAG,
                       "BSEC needs more than HAC_BSEC_INSTANCE_BYTES", fields,
                       sizeof fields / sizeof fields[0]);
        return HAL_ERR_NO_MEM;
    }

    uint32_t state_len = 0u;
    bool have_state = false;
    if (s->bsec_started) {
        const bsec_library_return_t r =
            bsec_get_state_m(s->instance, 0u, s->state, (uint32_t)sizeof s->state, s_work,
                             (uint32_t)sizeof s_work, &state_len);
        have_state = (r == BSEC_OK && state_len > 0u);
        if (r != BSEC_OK) {
            log_bsec(s, LOG_LEVEL_WARN, "BSEC state not saved", "bsec_get_state", r);
        }
    }

    bsec_library_return_t r = bsec_init_m(s->instance);
    if (r != BSEC_OK) {
        log_bsec(s, LOG_LEVEL_ERROR, "BSEC refused", "bsec_init", r);
        return HAL_ERR_IO;
    }
    s->bsec_started = true;

    bsec_version_t version;
    memset(&version, 0, sizeof version);
    (void)bsec_get_version_m(s->instance, &version);
    format_version(&version, s->bsec_version_text);

    r = bsec_set_configuration_m(s->instance, bsec_config_iaq, (uint32_t)sizeof bsec_config_iaq,
                                 s_work, (uint32_t)sizeof s_work);
    if (r != BSEC_OK) {
        log_bsec(s, LOG_LEVEL_ERROR, "BSEC refused its configuration", "bsec_set_configuration", r);
        return HAL_ERR_UNSUPPORTED;  // a configuration for another version or solution
    }

    s->state_restored = false;
    if (have_state) {
        r = bsec_set_state_m(s->instance, s->state, state_len, s_work, (uint32_t)sizeof s_work);
        s->state_restored = (r == BSEC_OK);
        if (r != BSEC_OK) {
            // Whatever it took of the blob, start clean rather than half-restored.
            log_bsec(s, LOG_LEVEL_WARN, "BSEC state not restored; starting afresh",
                     "bsec_set_state", r);
            r = bsec_init_m(s->instance);
            if (r == BSEC_OK) {
                r = bsec_set_configuration_m(s->instance, bsec_config_iaq,
                                             (uint32_t)sizeof bsec_config_iaq, s_work,
                                             (uint32_t)sizeof s_work);
            }
            if (r != BSEC_OK) {
                log_bsec(s, LOG_LEVEL_ERROR, "BSEC refused", "bsec_init", r);
                return HAL_ERR_IO;
            }
        }
    }

    bsec_sensor_configuration_t requested[SUBSCRIBED_COUNT];
    for (size_t i = 0u; i < SUBSCRIBED_COUNT; ++i) {
        requested[i].sample_rate = BSEC_SAMPLE_RATE_LP;
        requested[i].sensor_id = SUBSCRIBED[i];
    }
    bsec_sensor_configuration_t required[BSEC_MAX_PHYSICAL_SENSOR];
    uint8_t n_required = BSEC_MAX_PHYSICAL_SENSOR;
    r = bsec_update_subscription_m(s->instance, requested, (uint8_t)SUBSCRIBED_COUNT, required,
                                   &n_required);
    if (r != BSEC_OK) {
        // A warning too: BSEC_W_SU_SAMPLERATEMISMATCH means the configuration
        // linked is for another rate than HAC_BSEC_PERIOD_MS (cmake/bsec.cmake).
        log_bsec(s, LOG_LEVEL_ERROR, "BSEC refused the subscription", "bsec_update_subscription",
                 r);
        return HAL_ERR_UNSUPPORTED;
    }
    return HAL_OK;
}

// --- the measurement BSEC asks for --------------------------------------------

static void log_heater_change(bme68x_bsec_source_t *s, uint16_t temp_c, uint16_t dur_ms)
{
    if (temp_c == s->heater_temp_c && dur_ms == s->heater_dur_ms) {
        return;
    }
    s->heater_temp_c = temp_c;
    s->heater_dur_ms = dur_ms;
    const log_field_t fields[] = {
        log_u64("heater_c", temp_c),
        log_u64("heater_ms", dur_ms),
    };
    port_log_write(s->cfg.log, LOG_LEVEL_INFO, LOG_TAG, "heater profile", fields,
                   sizeof fields / sizeof fields[0]);
}

// Configures the sensor as `set` says and starts a forced measurement.
static hal_status_t measure(bme68x_bsec_source_t *s, const bsec_bme_settings_t *set, uint64_t now)
{
    // Asleep first: Bosch's bme68x_set_op_mode(), under every call below,
    // would otherwise wait for it with no bound (bme68x_common.h).
    hal_status_t status = bme68x_check_asleep(&s->dev);
    if (status != HAL_OK) {
        return status;
    }

    s->conf.os_temp = set->temperature_oversampling;
    s->conf.os_pres = set->pressure_oversampling;
    s->conf.os_hum = set->humidity_oversampling;
    s->conf.filter = HAC_BME68X_FILTER;
    s->conf.odr = BME68X_ODR_NONE;
    status = bme68x_vendor_status(&s->dev, bme68x_set_conf(&s->conf, &s->dev));
    if (status != HAL_OK) {
        return status;
    }

    memset(&s->heater, 0, sizeof s->heater);
    s->heater.enable = (set->run_gas != 0u) ? BME68X_ENABLE : BME68X_DISABLE;
    s->heater.heatr_temp = set->heater_temperature;
    s->heater.heatr_dur = set->heater_duration;
    status = bme68x_vendor_status(&s->dev,
                                  bme68x_set_heatr_conf(BME68X_FORCED_MODE, &s->heater, &s->dev));
    if (status != HAL_OK) {
        return status;
    }
    status = bme68x_vendor_status(&s->dev, bme68x_set_op_mode(BME68X_FORCED_MODE, &s->dev));
    if (status != HAL_OK) {
        return status;
    }

    // Bosch's estimate of the conversions, plus the heater's time, as in
    // bme68x_source.c.
    const uint32_t heater_us =
        (set->run_gas != 0u) ? (uint32_t)set->heater_duration * US_PER_MS : 0u;
    const uint32_t us = bme68x_get_meas_dur(BME68X_FORCED_MODE, &s->conf, &s->dev) + heater_us;
    const uint32_t ms = us / US_PER_MS + ((us % US_PER_MS) != 0u ? 1u : 0u);

    s->measuring = true;
    s->measured_at_ms = now;
    s->ready_at_ms = now + ms;
    s->next_read_ms = s->ready_at_ms;
    s->process_data = set->process_data;
    s->measurements++;
    log_heater_change(s, set->heater_temperature, set->heater_duration);
    return HAL_OK;
}

// BSEC's call, made now: whatever it asks for begins.
static hal_status_t call_bsec(bme68x_bsec_source_t *s, uint64_t now)
{
    bsec_bme_settings_t set;
    memset(&set, 0, sizeof set);
    const bsec_library_return_t r = bsec_sensor_control_m(s->instance, ns_of(now), &set);
    if (r < BSEC_OK) {
        log_bsec(s, LOG_LEVEL_WARN, "BSEC refused the call", "bsec_sensor_control", r);
        return HAL_ERR_IO;
    }
    if (r == BSEC_W_SC_CALL_TIMING_VIOLATION) {
        if (s->late_calls < UINT32_MAX) {
            s->late_calls++;
        }
        const log_field_t fields[] = {
            log_u64("late_ms",
                    (s->next_call_known && now > s->next_call_ms) ? now - s->next_call_ms : 0u),
            log_u64("tolerance_ms", HAC_BSEC_LATE_TOLERANCE_MS),
            log_bool("first_since_init", !s->next_call_known),
            log_u64("late_calls", s->late_calls),
        };
        port_log_write(s->cfg.log, LOG_LEVEL_WARN, LOG_TAG, "BSEC called late", fields,
                       sizeof fields / sizeof fields[0]);
    } else if (r > BSEC_OK) {
        log_bsec(s, LOG_LEVEL_WARN, "BSEC warned", "bsec_sensor_control", r);
    }

    s->next_call_ms = ms_of(set.next_call);
    s->next_call_known = true;
    if (set.trigger_measurement == 0u) {
        return HAL_OK;  // nothing to measure at this call
    }
    if (set.op_mode != BME68X_FORCED_MODE) {
        // BSEC's IAQ solution asks a BME680 for forced mode only; parallel
        // mode is the BME688's gas scanner, which this source does not run.
        const log_field_t fields[] = {log_u64("op_mode", set.op_mode)};
        port_log_write(s->cfg.log, LOG_LEVEL_WARN, LOG_TAG, "BSEC asked for an unsupported mode",
                       fields, 1u);
        return HAL_ERR_UNSUPPORTED;
    }
    return measure(s, &set, now);
}

// Makes BSEC's call if its time has come and no measurement is waiting to be
// collected. Never early.
static hal_status_t step(bme68x_bsec_source_t *s, uint64_t now)
{
    if (s->measuring || now < s->next_call_ms) {
        return HAL_OK;
    }
    return call_bsec(s, now);
}

// --- BSEC's outputs ----------------------------------------------------------

typedef struct {
    bool have_temperature, have_humidity, have_pressure, have_gas, have_iaq;
    bool have_stabilization, have_run_in;
    float temperature_c, humidity_rh, pressure_pa, gas_ohm, iaq;
    uint8_t iaq_accuracy;
    bool stabilized, run_in_done;
} outputs_t;

static void collect_outputs(const bsec_output_t *o, uint8_t n, outputs_t *out)
{
    memset(out, 0, sizeof *out);
    for (uint8_t i = 0u; i < n; ++i) {
        switch (o[i].sensor_id) {
            case BSEC_OUTPUT_IAQ:
                out->have_iaq = true;
                out->iaq = o[i].signal;
                out->iaq_accuracy = o[i].accuracy;
                break;
            case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE:
                out->have_temperature = true;
                out->temperature_c = o[i].signal;
                break;
            case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY:
                out->have_humidity = true;
                out->humidity_rh = o[i].signal;
                break;
            case BSEC_OUTPUT_RAW_PRESSURE:
                out->have_pressure = true;
                out->pressure_pa = o[i].signal;
                break;
            case BSEC_OUTPUT_RAW_GAS:
                out->have_gas = true;
                out->gas_ohm = o[i].signal;
                break;
            case BSEC_OUTPUT_STABILIZATION_STATUS:
                out->have_stabilization = true;
                out->stabilized = o[i].signal > 0.5f;
                break;
            case BSEC_OUTPUT_RUN_IN_STATUS:
                out->have_run_in = true;
                out->run_in_done = o[i].signal > 0.5f;
                break;
            default:
                break;
        }
    }
}

// What changed in BSEC's verdicts, said once per change.
static void log_verdicts(bme68x_bsec_source_t *s, const outputs_t *o)
{
    if (o->have_iaq && (!s->have_accuracy || o->iaq_accuracy != s->iaq_accuracy)) {
        const log_field_t fields[] = {
            log_str("from", s->have_accuracy ? bme68x_bsec_accuracy_name(s->iaq_accuracy) : "-"),
            log_str("to", bme68x_bsec_accuracy_name(o->iaq_accuracy)),
            log_f32("iaq", o->iaq),
            log_u64("measurements", s->measurements),
        };
        port_log_write(s->cfg.log, LOG_LEVEL_INFO, LOG_TAG, "iaq accuracy", fields,
                       sizeof fields / sizeof fields[0]);
        s->have_accuracy = true;
        s->iaq_accuracy = o->iaq_accuracy;
    }
    const bool stabilized = o->have_stabilization ? o->stabilized : s->stabilized;
    const bool run_in_done = o->have_run_in ? o->run_in_done : s->run_in_done;
    if ((o->have_stabilization || o->have_run_in) &&
        (!s->have_gas_state || stabilized != s->stabilized || run_in_done != s->run_in_done)) {
        const log_field_t fields[] = {
            log_bool("stabilized", stabilized),
            log_bool("run_in_done", run_in_done),
            log_u64("measurements", s->measurements),
        };
        port_log_write(s->cfg.log, LOG_LEVEL_INFO, LOG_TAG, "gas sensor status", fields,
                       sizeof fields / sizeof fields[0]);
        s->have_gas_state = true;
        s->stabilized = stabilized;
        s->run_in_done = run_in_done;
    }
}

static void fill_sample(bme68x_bsec_source_t *s, sensor_sample_t *out, const outputs_t *o,
                        bool gas_valid, uint64_t now)
{
    clear_sample(out);
    const uint8_t id = s->cfg.id;

    out->temperature_c = o->have_temperature ? measurement_ok(o->temperature_c, now, id)
                                             : measurement_status(VAL_ERROR, now, id);
    out->humidity_rh = (o->have_humidity && within(o->humidity_rh, HAC_BME68X_HUMIDITY_MIN_RH,
                                                   HAC_BME68X_HUMIDITY_MAX_RH))
                           ? measurement_ok(o->humidity_rh, now, id)
                           : measurement_status(VAL_ERROR, now, id);

    const float p_hpa = o->pressure_pa / PA_PER_HPA;
    out->pressure_hpa = (o->have_pressure &&
                         within(p_hpa, HAC_BME68X_PRESSURE_MIN_HPA, HAC_BME68X_PRESSURE_MAX_HPA))
                            ? measurement_ok(p_hpa, now, id)
                            : measurement_status(VAL_ERROR, now, id);

    const value_status_t gas_status =
        bme68x_bsec_gas_status(gas_valid && o->have_gas, s->stabilized, s->run_in_done);
    out->gas_resistance_ohm = (gas_status == VAL_OK) ? measurement_ok(o->gas_ohm, now, id)
                                                     : measurement_status(gas_status, now, id);

    const value_status_t iaq_status =
        o->have_iaq ? bme68x_bsec_iaq_status(o->iaq_accuracy) : VAL_ERROR;
    out->iaq = (iaq_status == VAL_OK) ? measurement_ok(o->iaq, now, id)
                                      : measurement_status(iaq_status, now, id);
    if (o->have_iaq) {
        out->iaq.accuracy = bme68x_bsec_iaq_accuracy(o->iaq_accuracy);
    }
}

// The finished measurement handed to BSEC, and its outputs into `out`.
static hal_status_t process(bme68x_bsec_source_t *s, sensor_sample_t *out,
                            const struct bme68x_data *d)
{
    const uint64_t now = now_ms(s);
    const bool gas_valid = (d->status & BME68X_GASM_VALID_MSK) != 0u;
    const bool heat_stab = (d->status & BME68X_HEAT_STAB_MSK) != 0u;

    // Outside the operating range humidity and pressure go with the
    // temperature (bme68x_source.h), and BSEC is not handed what the sensor
    // cannot have measured.
    if (!within(d->temperature, HAC_BME68X_TEMPERATURE_MIN_C, HAC_BME68X_TEMPERATURE_MAX_C)) {
        every_field_failed(s, out, now);
        return HAL_OK;
    }

    // Gas the sensor did not mark valid is not a reading and is not handed
    // over. Heater stability is not required: Bosch's own integration
    // (bsec_integration.c in the BSEC archive) hands BSEC every valid gas
    // value, and BSEC's run-in judges the element. Logged, once per streak.
    if (!gas_valid || !heat_stab) {
        if (!s->gas_failing) {
            const log_field_t fields[] = {
                log_bool("gas_valid", gas_valid),
                log_bool("heat_stab", heat_stab),
                log_u64("heater_c", s->heater_temp_c),
                log_u64("heater_ms", s->heater_dur_ms),
            };
            port_log_write(s->cfg.log, gas_valid ? LOG_LEVEL_INFO : LOG_LEVEL_WARN, LOG_TAG,
                           gas_valid ? "gas valid, heater not stable" : "gas not measured", fields,
                           sizeof fields / sizeof fields[0]);
            s->gas_failing = true;
        }
    } else {
        s->gas_failing = false;
    }

    const int64_t ts = ns_of(s->measured_at_ms);
    bsec_input_t inputs[BSEC_MAX_PHYSICAL_SENSOR];
    uint8_t n = 0u;
    if (wants(s->process_data, BSEC_INPUT_HEATSOURCE)) {
        inputs[n++] = (bsec_input_t){ts, HAC_BSEC_DEVICE_HEAT_C, 1u, BSEC_INPUT_HEATSOURCE};
    }
    if (wants(s->process_data, BSEC_INPUT_TEMPERATURE)) {
        inputs[n++] = (bsec_input_t){ts, d->temperature, 1u, BSEC_INPUT_TEMPERATURE};
    }
    if (wants(s->process_data, BSEC_INPUT_HUMIDITY)) {
        inputs[n++] = (bsec_input_t){ts, d->humidity, 1u, BSEC_INPUT_HUMIDITY};
    }
    if (wants(s->process_data, BSEC_INPUT_PRESSURE)) {
        inputs[n++] = (bsec_input_t){ts, d->pressure, 1u, BSEC_INPUT_PRESSURE};
    }
    if (wants(s->process_data, BSEC_INPUT_GASRESISTOR) && gas_valid) {
        inputs[n++] = (bsec_input_t){ts, d->gas_resistance, 1u, BSEC_INPUT_GASRESISTOR};
    }

    bsec_output_t outputs[BSEC_NUMBER_OUTPUTS];
    uint8_t n_out = BSEC_NUMBER_OUTPUTS;
    const bsec_library_return_t r = bsec_do_steps_m(s->instance, inputs, n, outputs, &n_out);
    if (r < BSEC_OK) {
        log_bsec(s, LOG_LEVEL_WARN, "BSEC refused the measurement", "bsec_do_steps", r);
        return fail(s, out, HAL_ERR_IO, "bsec_do_steps");
    }
    if (r > BSEC_OK) {
        if (!s->bsec_warning) {
            log_bsec(s, LOG_LEVEL_WARN, "BSEC warned", "bsec_do_steps", r);
            s->bsec_warning = true;
        }
    } else {
        s->bsec_warning = false;
    }

    outputs_t o;
    collect_outputs(outputs, n_out, &o);
    log_verdicts(s, &o);
    fill_sample(s, out, &o, gas_valid, now);
    if (s->failing) {
        s->failing = false;
        port_log_write(s->cfg.log, LOG_LEVEL_INFO, LOG_TAG, "reading again", NULL, 0u);
    }
    return HAL_OK;
}

// --- sensor_source_t ---------------------------------------------------------

static hal_status_t src_init(sensor_source_t *self)
{
    bme68x_bsec_source_t *s = self_of(self);
    s->initialized = false;
    s->running = false;
    s->next_call_known = false;
    s->next_call_ms = 0u;
    s->measuring = false;
    s->ready_at_ms = 0u;
    s->next_read_ms = 0u;
    s->heater_temp_c = 0u;
    s->heater_dur_ms = 0u;
    s->have_accuracy = false;
    s->have_gas_state = false;
    s->stabilized = false;
    s->run_in_done = false;
    s->failing = false;
    s->gas_failing = false;
    s->bsec_warning = false;
    s->chip_id = 0u;
    s->variant_id = 0u;
    bme68x_format_id(0u, s->chip_id_text);
    bme68x_format_id(0u, s->variant_id_text);

    // Start-up time, soft reset, ids, calibration, asleep (bme68x_common.h).
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
    if (s->variant_id != BME68X_VARIANT_GAS_LOW) {
        // The configuration is a BME680's (cmake/bsec.cmake): BSEC runs, but
        // its heater profile and models are not this part's.
        const log_field_t fields[] = {
            log_str("variant", bme68x_variant_name(s->variant_id)),
            log_str("config", HAC_BSEC_CONFIG_NAME),
        };
        port_log_write(s->cfg.log, LOG_LEVEL_WARN, LOG_TAG, "BSEC configuration is a BME680's",
                       fields, sizeof fields / sizeof fields[0]);
    }

    memset(&s->conf, 0, sizeof s->conf);
    status = start_bsec(s);
    if (status != HAL_OK) {
        return status;
    }

    const log_field_t fields[] = {
        log_u64("address", s->cfg.address),
        log_str("chip_id", s->chip_id_text),
        log_str("variant", bme68x_variant_name(s->variant_id)),
        log_str("bsec", s->bsec_version_text),
        log_str("config", HAC_BSEC_CONFIG_NAME),
        log_str("state", s->state_restored ? "kept_since_boot" : "fresh"),
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
    bme68x_bsec_source_t *s = self_of(self);
    if (!s->initialized) {
        return HAL_ERR_INVALID_ARG;
    }
    s->running = true;
    return step(s, now_ms(s));
}

static uint32_t src_ready_in_ms(const sensor_source_t *self)
{
    const bme68x_bsec_source_t *s = (const bme68x_bsec_source_t *)self->impl;
    if (!s->initialized || !s->running) {
        return 0u;
    }
    const uint64_t now = now_ms(s);
    const uint64_t at = s->measuring ? s->next_read_ms : s->next_call_ms;
    if (at <= now) {
        return 0u;
    }
    const uint64_t wait = at - now;
    return wait > UINT32_MAX ? UINT32_MAX : (uint32_t)wait;
}

static hal_status_t src_read(sensor_source_t *self, sensor_sample_t *out)
{
    bme68x_bsec_source_t *s = self_of(self);
    if (!s->initialized || !s->running) {
        return HAL_ERR_NOT_READY;
    }
    if (!s->measuring) {
        const hal_status_t status = step(s, now_ms(s));
        if (status != HAL_OK) {
            return fail(s, out, status, "measure");
        }
        return HAL_ERR_NOT_READY;  // BSEC's measurement has begun, or its time has not come
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

    s->measuring = false;  // collected; BSEC's next call starts another
    return process(s, out, &data);
}

static hal_status_t src_suspend(sensor_source_t *self)
{
    bme68x_bsec_source_t *s = self_of(self);
    if (!s->initialized) {
        return HAL_OK;
    }
    if (s->measuring && now_ms(s) < s->ready_at_ms) {
        return HAL_ERR_BUSY;  // a measurement is running; ready_in_ms() says how long
    }
    // Between forced measurements the sensor sleeps by itself; a reading not
    // collected is dropped, and BSEC sees a gap.
    s->measuring = false;
    s->running = false;
    return HAL_OK;
}

static void src_deinit(sensor_source_t *self)
{
    bme68x_bsec_source_t *s = self_of(self);
    s->initialized = false;
    s->running = false;
    s->measuring = false;
}

hal_status_t bme68x_bsec_source_setup(bme68x_bsec_source_t *s,
                                      const bme68x_bsec_source_config_t *cfg)
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
    s->port.caps = SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY | SENS_CAP_PRESSURE | SENS_CAP_GAS_RES |
                   SENS_CAP_IAQ;
    s->port.init = src_init;
    s->port.start = src_start;
    s->port.ready_in_ms = src_ready_in_ms;
    s->port.read = src_read;
    s->port.suspend = src_suspend;
    s->port.deinit = src_deinit;
    s->port.impl = s;
    return HAL_OK;
}
