// SPDX-License-Identifier: Apache-2.0
//
// scd4x_source -- Sensirion's own driver behind sensor_source_t -- over the
// fake bus and the SCD41 model, at its boundaries: every way the bus and the
// sensor can fail, the datasheet's range limits, the warm-up, the reset that
// leaves the sensor measuring, and the HAL glue under it all. The happy path
// is the contract's (tests/contract/test_port_sensor_contract.c).

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_i2c_fake.h"
#include "hac_test.h"
#include "i2c_models/scd41_model.h"
#include "scd4x_hal.h"
#include "scd4x_i2c.h"
#include "scd4x_limits.h"
#include "scd4x_source.h"
#include "sensirion_common.h"
#include "sensirion_i2c.h"
#include "sensirion_i2c_hal.h"

#define ADDR 0x62u
#define SOURCE_ID 3u

// --- a log that remembers ----------------------------------------------------

#define LOG_LINES 16u
#define LOG_LINE_LEN 200u

typedef struct {
    port_log_t port;
    char lines[LOG_LINES][LOG_LINE_LEN];
    size_t count;
} capture_log_t;

static void capture_write(const port_log_t *self, log_level_t level, const char *tag,
                          const char *msg, const log_field_t *fields, size_t field_count)
{
    (void)level;
    capture_log_t *c = (capture_log_t *)self->impl;
    if (c->count >= LOG_LINES) {
        return;
    }
    char *line = c->lines[c->count++];
    size_t used = (size_t)snprintf(line, LOG_LINE_LEN, "%s: %s", tag, msg != NULL ? msg : "");
    for (size_t i = 0; i < field_count && used < LOG_LINE_LEN; i++) {
        const log_field_t *f = &fields[i];
        if (f->kind == LOG_FIELD_STR) {
            used += (size_t)snprintf(line + used, LOG_LINE_LEN - used, " %s=%s", f->key, f->v.str);
        } else if (f->kind == LOG_FIELD_U64) {
            used += (size_t)snprintf(line + used, LOG_LINE_LEN - used, " %s=%llu", f->key,
                                     (unsigned long long)f->v.u64);
        } else if (f->kind == LOG_FIELD_BOOL) {
            used += (size_t)snprintf(line + used, LOG_LINE_LEN - used, " %s=%s", f->key,
                                     f->v.b ? "true" : "false");
        }
    }
}

static int log_count(const capture_log_t *c, const char *needle)
{
    int n = 0;
    for (size_t i = 0; i < c->count; i++) {
        n += (strstr(c->lines[i], needle) != NULL) ? 1 : 0;
    }
    return n;
}

// --- the rig -------------------------------------------------------------------

typedef struct {
    float co2_ppm;
    float temperature_c;
    float humidity_rh;
} air_t;

static void reading_from_air(void *ctx, uint64_t at_ms, scd41_model_reading_t *out)
{
    (void)at_ms;
    const air_t *air = (const air_t *)ctx;
    out->co2_ppm = air->co2_ppm;
    out->temperature_c = air->temperature_c;
    out->humidity_rh = air->humidity_rh;
}

typedef struct {
    adp_clock_virtual_t clock;
    adp_i2c_fake_t bus;
    scd41_model_t sensor;
    air_t air;
    capture_log_t log;
    scd4x_source_t source;
    sensor_source_t *src;
} rig_t;

// Static: the rig is large, and every test makes one.
static rig_t g_rig;

static rig_t *rig_make(scd4x_mode_t mode, bool attach)
{
    rig_t *r = &g_rig;
    memset(r, 0, sizeof *r);
    adp_clock_virtual_init(&r->clock, 1.0);
    adp_i2c_fake_init(&r->bus, &r->clock);
    scd41_model_init(&r->sensor, 0u);
    r->air = (air_t){.co2_ppm = 650.0f, .temperature_c = 21.5f, .humidity_rh = 45.0f};
    scd41_model_set_reading(&r->sensor, reading_from_air, &r->air);
    if (attach) {
        (void)adp_i2c_fake_attach(&r->bus, ADDR, scd41_model_port(&r->sensor));
    }
    r->log.port.write = capture_write;
    r->log.port.impl = &r->log;

    const scd4x_source_config_t cfg = {
        .bus = adp_i2c_fake_port(&r->bus),
        .clock = adp_clock_virtual_port(&r->clock),
        .log = &r->log.port,
        .address = ADDR,
        .id = SOURCE_ID,
        .mode = mode,
    };
    (void)scd4x_source_setup(&r->source, &cfg);
    r->src = &r->source.port;
    return r;
}

static void rig_done(rig_t *r)
{
    r->src->deinit(r->src);
}

static void advance(rig_t *r, uint64_t ms)
{
    adp_clock_virtual_advance(&r->clock, ms);
}

// Enough "not yet"s to cover a whole interval at the retry rate: a sensor
// that skipped a reading has the next one up to an interval later.
#define READ_ATTEMPTS (SCD4X_PERIODIC_INTERVAL_MS / SCD4X_NOT_READY_RETRY_MS + 4u)

// Waits what ready_in_ms() asks and reads, until the source answers
// something other than "not yet".
static hal_status_t read_when_ready(rig_t *r, sensor_sample_t *out)
{
    hal_status_t status = HAL_ERR_NOT_READY;
    for (unsigned attempt = 0; attempt < READ_ATTEMPTS && status == HAL_ERR_NOT_READY; attempt++) {
        const uint32_t wait = r->src->ready_in_ms(r->src);
        advance(r, wait > 0u ? wait : 1u);
        status = r->src->read(r->src, out);
    }
    return status;
}

// Past the warm-up, so statuses say VAL_OK or VAL_ERROR.
static void start_and_warm_up(rig_t *r)
{
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);
    advance(r, SCD4X_WARMUP_MS);
    sensor_sample_t discard;
    (void)r->src->read(r->src, &discard);  // empty the sensor's buffer
}

static bool all_error(const sensor_sample_t *s)
{
    return s->co2_ppm.status == VAL_ERROR && s->temperature_c.status == VAL_ERROR &&
           s->humidity_rh.status == VAL_ERROR && s->pressure_hpa.status == VAL_UNAVAILABLE;
}

// --- init ----------------------------------------------------------------------

static void init_logs_the_serial_number_and_the_variant(void)
{
    rig_t *r = rig_make(SCD4X_MODE_PERIODIC, true);
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(log_count(&r->log, "scd4x: sensor ready"), 1);
    HAC_CHECK_EQ_INT(log_count(&r->log, "serial=0xf8969f073bbe"), 1);  // datasheet Table 27
    HAC_CHECK_EQ_INT(log_count(&r->log, "variant=SCD41"), 1);
    HAC_CHECK_EQ_INT(log_count(&r->log, "mode=periodic"), 1);
    HAC_CHECK_EQ_INT(log_count(&r->log, "address=98"), 1);
    HAC_CHECK_EQ_INT(log_count(&r->log, "found_measuring=false"), 1);  // freshly powered
    HAC_CHECK(r->source.variant == SCD4X_VARIANT_SCD41);
    rig_done(r);
}

static void init_stops_a_sensor_a_reset_left_measuring(void)
{
    rig_t *r = rig_make(SCD4X_MODE_PERIODIC, true);
    scd41_model_set_measuring(&r->sensor, 0u);  // a software reset kept its supply
    advance(r, 12345u);

    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    // One refusal, on purpose: the serial number asked before the stop.
    HAC_CHECK_EQ_INT((int)r->sensor.stats.rejected, 1);
    HAC_CHECK(r->source.found_measuring);
    HAC_CHECK_EQ_INT(log_count(&r->log, "found_measuring=true"), 1);
    HAC_CHECK(r->sensor.stats.history_len >= 4u);
    HAC_CHECK_EQ_INT(r->sensor.stats.history[0], 0x3F86);  // stop_periodic_measurement first
    HAC_CHECK_EQ_INT(r->sensor.stats.history[1], 0x3646);  // then reinit
    HAC_CHECK_EQ_INT(r->sensor.stats.history[2], 0x3682);  // then the serial number
    HAC_CHECK_EQ_INT(r->sensor.stats.history[3], 0x202F);  // and the variant
    HAC_CHECK(r->sensor.state == SCD41_MODEL_IDLE);
    rig_done(r);
}

static void without_the_stop_a_measuring_sensor_refuses_init(void)
{
    // What init()'s first command is for: Sensirion's driver, asked for the
    // serial number while the sensor measures, is refused.
    rig_t *r = rig_make(SCD4X_MODE_PERIODIC, true);
    scd41_model_set_measuring(&r->sensor, 0u);
    advance(r, 100u);
    HAC_CHECK_EQ_INT(
        scd4x_hal_claim(r, adp_i2c_fake_port(&r->bus), adp_clock_virtual_port(&r->clock)), HAL_OK);
    scd4x_init(ADDR);
    uint16_t words[3];
    HAC_CHECK(scd4x_get_serial_number(words, 3u) != NO_ERROR);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.rejected, 1);
    scd4x_hal_release(r);
}

static void init_waits_out_the_power_up_time(void)
{
    // The supply has only just risen: the model refuses its address for
    // 30 ms. init() must not be refused.
    rig_t *r = rig_make(SCD4X_MODE_PERIODIC, true);
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.nacked_while_busy, 0);
    rig_done(r);
}

static void every_vendor_wait_is_at_least_the_datasheets(void)
{
    // The model refuses its address inside every execution time, so a wait
    // in Sensirion's code shorter than the datasheet's would show here.
    rig_t *r = rig_make(SCD4X_MODE_PERIODIC, true);
    start_and_warm_up(r);
    sensor_sample_t s;
    for (int i = 0; i < 5; i++) {
        HAC_CHECK_EQ_INT(read_when_ready(r, &s), HAL_OK);
    }
    HAC_CHECK_EQ_INT(scd4x_source_set_ambient_pressure(&r->source, 1000.0f), HAL_OK);
    HAC_CHECK_EQ_INT(r->src->suspend(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.nacked_while_busy, 0);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.rejected, 0);
    rig_done(r);
}

static void an_absent_sensor_fails_init_as_not_found(void)
{
    rig_t *r = rig_make(SCD4X_MODE_PERIODIC, false);
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_ERR_NOT_FOUND);
    HAC_CHECK_EQ_INT(log_count(&r->log, "step=stop_periodic_measurement status=not_found"), 1);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_NOT_READY);
    rig_done(r);
}

static void an_scd40_is_refused_single_shot_mode_but_measures_periodically(void)
{
    rig_t *r = rig_make(SCD4X_MODE_SINGLE_SHOT, true);
    r->sensor.variant_word = SCD41_MODEL_VARIANT_SCD40;
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_ERR_UNSUPPORTED);
    rig_done(r);

    r = rig_make(SCD4X_MODE_PERIODIC, true);
    r->sensor.variant_word = SCD41_MODEL_VARIANT_SCD40;
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(log_count(&r->log, "variant=SCD40"), 1);
    rig_done(r);
}

static void only_one_source_can_hold_the_hal(void)
{
    rig_t *r = rig_make(SCD4X_MODE_PERIODIC, true);
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);

    scd4x_source_t second;
    const scd4x_source_config_t cfg = r->source.cfg;
    HAC_CHECK_EQ_INT(scd4x_source_setup(&second, &cfg), HAL_OK);
    HAC_CHECK_EQ_INT(second.port.init(&second.port), HAL_ERR_BUSY);
    second.port.deinit(&second.port);

    rig_done(r);  // releases it
    HAC_CHECK_EQ_INT(second.port.init(&second.port), HAL_OK);
    second.port.deinit(&second.port);
}

// --- periodic readings ---------------------------------------------------------

static void readings_are_warmup_first_then_ok(void)
{
    rig_t *r = rig_make(SCD4X_MODE_PERIODIC, true);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(r->src->ready_in_ms(r->src), 0);  // not started
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(r->src->ready_in_ms(r->src), SCD4X_PERIODIC_INTERVAL_MS);
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_NOT_READY);  // too early

    HAC_CHECK_EQ_INT(read_when_ready(r, &s), HAL_OK);
    HAC_CHECK(s.co2_ppm.status == VAL_WARMUP);
    HAC_CHECK(s.temperature_c.status == VAL_WARMUP);
    HAC_CHECK(s.humidity_rh.status == VAL_WARMUP);
    HAC_CHECK_EQ_INT(s.co2_ppm.source_id, SOURCE_ID);

    advance(r, SCD4X_WARMUP_MS);
    HAC_CHECK_EQ_INT(read_when_ready(r, &s), HAL_OK);
    HAC_CHECK(s.co2_ppm.status == VAL_OK);
    HAC_CHECK_EQ_F32(s.co2_ppm.value, 650.0f, 0.0f);
    HAC_CHECK_EQ_F32(s.temperature_c.value, 21.5f, 0.01f);
    HAC_CHECK_EQ_F32(s.humidity_rh.value, 45.0f, 0.01f);
    HAC_CHECK_EQ_U64(s.co2_ppm.ts_ms, r->clock.now_ms);
    HAC_CHECK(s.pressure_hpa.status == VAL_UNAVAILABLE);
    HAC_CHECK(s.gas_resistance_ohm.status == VAL_UNAVAILABLE);
    rig_done(r);
}

static void a_sensor_with_nothing_new_is_not_ready(void)
{
    rig_t *r = rig_make(SCD4X_MODE_PERIODIC, true);
    start_and_warm_up(r);
    r->sensor.faults.hold_data = true;

    sensor_sample_t s;
    memset(&s, 0xA5, sizeof s);
    advance(r, SCD4X_PERIODIC_INTERVAL_MS);
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_NOT_READY);
    HAC_CHECK_EQ_INT(r->src->ready_in_ms(r->src), SCD4X_NOT_READY_RETRY_MS);
    uint8_t untouched[sizeof s];
    memset(untouched, 0xA5, sizeof untouched);
    HAC_CHECK(memcmp(&s, untouched, sizeof s) == 0);  // out is left alone

    r->sensor.faults.hold_data = false;
    HAC_CHECK_EQ_INT(read_when_ready(r, &s), HAL_OK);
    rig_done(r);
}

static void a_read_header_refused_after_the_command_is_not_yet_not_gone(void)
{
    // read_measurement acknowledged, its read header refused: the sensor has
    // nothing buffered (datasheet 3.6.2). That is "not yet", even past the
    // shot's window and its grace, where a refused address would be "gone".
    rig_t *r = rig_make(SCD4X_MODE_SINGLE_SHOT, true);
    r->sensor.faults.hold_data = true;
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);

    sensor_sample_t s;
    advance(r, SCD4X_SINGLE_SHOT_MS + SCD4X_BUSY_GRACE_MS);
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_NOT_READY);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.reads_without_data, 1);
    HAC_CHECK_EQ_INT(r->src->ready_in_ms(r->src), SCD4X_NOT_READY_RETRY_MS);
    HAC_CHECK_EQ_INT(log_count(&r->log, "read failed"), 0);

    r->sensor.faults.hold_data = false;
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);  // another shot
    HAC_CHECK_EQ_INT(read_when_ready(r, &s), HAL_OK);
    rig_done(r);
}

// --- failures ------------------------------------------------------------------

static void a_bad_crc_is_an_io_error_and_every_field_an_error(void)
{
    rig_t *r = rig_make(SCD4X_MODE_PERIODIC, true);
    start_and_warm_up(r);
    advance(r, SCD4X_PERIODIC_INTERVAL_MS);
    r->sensor.faults.corrupt_crc = 1u;

    sensor_sample_t s;
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_IO);
    HAC_CHECK(all_error(&s));
    HAC_CHECK_EQ_U64(s.co2_ppm.ts_ms, r->clock.now_ms);
    HAC_CHECK_EQ_INT(s.co2_ppm.source_id, SOURCE_ID);
    // Not polled faster than the sensor measures.
    HAC_CHECK_EQ_INT(r->src->ready_in_ms(r->src), SCD4X_PERIODIC_INTERVAL_MS);

    HAC_CHECK_EQ_INT(read_when_ready(r, &s), HAL_OK);  // and it recovers
    HAC_CHECK(s.co2_ppm.status == VAL_OK);
    rig_done(r);
}

static void a_bad_crc_on_the_measurement_itself_is_an_io_error(void)
{
    // Single shot reads no data-ready word first, so the corrupted word is
    // the measurement's.
    rig_t *r = rig_make(SCD4X_MODE_SINGLE_SHOT, true);
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);
    advance(r, SCD4X_SINGLE_SHOT_MS);
    r->sensor.faults.corrupt_crc = 1u;
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_IO);
    HAC_CHECK(all_error(&s));
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_NOT_READY);  // the shot is gone
    rig_done(r);
}

static void a_byte_refused_mid_transfer_is_an_io_error(void)
{
    rig_t *r = rig_make(SCD4X_MODE_PERIODIC, true);
    start_and_warm_up(r);
    advance(r, SCD4X_PERIODIC_INTERVAL_MS);
    r->sensor.faults.nack_data = 1u;

    sensor_sample_t s;
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_IO);
    HAC_CHECK(all_error(&s));
    rig_done(r);
}

static void a_sensor_that_stops_answering_is_not_found_and_logged_once(void)
{
    rig_t *r = rig_make(SCD4X_MODE_PERIODIC, true);
    start_and_warm_up(r);

    sensor_sample_t s;
    r->sensor.faults.nack_address = 2u;
    advance(r, SCD4X_PERIODIC_INTERVAL_MS);
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_NOT_FOUND);
    HAC_CHECK(all_error(&s));
    advance(r, r->src->ready_in_ms(r->src));
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_NOT_FOUND);
    HAC_CHECK_EQ_INT(log_count(&r->log, "read failed"), 1);

    HAC_CHECK_EQ_INT(read_when_ready(r, &s), HAL_OK);
    HAC_CHECK_EQ_INT(log_count(&r->log, "reading again"), 1);
    rig_done(r);
}

static void a_stalled_bus_is_a_timeout(void)
{
    rig_t *r = rig_make(SCD4X_MODE_PERIODIC, true);
    start_and_warm_up(r);
    advance(r, SCD4X_PERIODIC_INTERVAL_MS);
    adp_i2c_fake_hold_scl(&r->bus, true);
    const uint64_t before = r->clock.now_ms;

    sensor_sample_t s;
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_TIMEOUT);
    HAC_CHECK(all_error(&s));
    HAC_CHECK_EQ_U64(r->clock.now_ms - before, SCD4X_TRANSFER_TIMEOUT_MS);  // one deadline
    adp_i2c_fake_hold_scl(&r->bus, false);
    rig_done(r);
}

// --- single shot: busy, not absent -------------------------------------------

static void a_single_shot_is_not_disturbed_while_it_measures(void)
{
    rig_t *r = rig_make(SCD4X_MODE_SINGLE_SHOT, true);
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(r->src->ready_in_ms(r->src), SCD4X_SINGLE_SHOT_MS);

    const uint32_t transfers = r->bus.transfers;
    sensor_sample_t s;
    advance(r, SCD4X_SINGLE_SHOT_MS - 1u);
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_NOT_READY);
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_ERR_BUSY);
    HAC_CHECK_EQ_INT(scd4x_source_set_ambient_pressure(&r->source, 1013.0f), HAL_ERR_BUSY);
    HAC_CHECK_EQ_INT(r->src->suspend(r->src), HAL_ERR_BUSY);
    HAC_CHECK_EQ_INT((int)(r->bus.transfers - transfers), 0);  // the bus was left alone

    advance(r, 1u);
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_OK);
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_NOT_READY);  // one shot, one reading
    HAC_CHECK_EQ_INT((int)r->sensor.stats.nacked_while_busy, 0);
    rig_done(r);
}

static void a_slow_single_shot_is_busy_within_the_grace(void)
{
    rig_t *r = rig_make(SCD4X_MODE_SINGLE_SHOT, true);
    r->sensor.faults.single_shot_overrun_ms = SCD4X_BUSY_GRACE_MS / 2u;
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);

    sensor_sample_t s;
    advance(r, SCD4X_SINGLE_SHOT_MS);
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_NOT_READY);  // refused: still measuring
    HAC_CHECK_EQ_INT(r->sensor.stats.nacked_while_busy, 1);
    HAC_CHECK_EQ_INT(read_when_ready(r, &s), HAL_OK);
    rig_done(r);
}

static void a_single_shot_refused_past_the_grace_is_not_found(void)
{
    rig_t *r = rig_make(SCD4X_MODE_SINGLE_SHOT, true);
    r->sensor.faults.single_shot_overrun_ms = 3u * SCD4X_BUSY_GRACE_MS;
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);

    sensor_sample_t s;
    advance(r, SCD4X_SINGLE_SHOT_MS);
    hal_status_t status = HAL_ERR_NOT_READY;
    while (status == HAL_ERR_NOT_READY && r->clock.now_ms < 2u * SCD4X_SINGLE_SHOT_MS) {
        status = r->src->read(r->src, &s);
        advance(r, r->src->ready_in_ms(r->src));
    }
    HAC_CHECK_EQ_INT(status, HAL_ERR_NOT_FOUND);
    HAC_CHECK(all_error(&s));
    rig_done(r);
}

// --- ranges --------------------------------------------------------------------

static sensor_sample_t reading_of(rig_t *r, float co2, float t, float rh)
{
    r->air = (air_t){.co2_ppm = co2, .temperature_c = t, .humidity_rh = rh};
    sensor_sample_t s;
    memset(&s, 0, sizeof s);
    advance(r, SCD4X_PERIODIC_INTERVAL_MS);
    HAC_CHECK_EQ_INT(read_when_ready(r, &s), HAL_OK);
    return s;
}

static void values_outside_the_datasheets_range_are_errors(void)
{
    rig_t *r = rig_make(SCD4X_MODE_PERIODIC, true);
    start_and_warm_up(r);

    sensor_sample_t s = reading_of(r, 0.0f, 21.0f, 40.0f);
    HAC_CHECK(s.co2_ppm.status == VAL_ERROR);  // 0 ppm: "no CO2 value"
    HAC_CHECK(s.temperature_c.status == VAL_OK);
    s = reading_of(r, (float)SCD4X_CO2_MIN_PPM, 21.0f, 40.0f);
    HAC_CHECK(s.co2_ppm.status == VAL_OK);
    s = reading_of(r, (float)SCD4X_CO2_MAX_PPM, 21.0f, 40.0f);
    HAC_CHECK(s.co2_ppm.status == VAL_OK);
    HAC_CHECK_EQ_F32(s.co2_ppm.value, 40000.0f, 0.0f);
    s = reading_of(r, (float)SCD4X_CO2_MAX_PPM + 1.0f, 21.0f, 40.0f);
    HAC_CHECK(s.co2_ppm.status == VAL_ERROR);

    // -10 C and 60 C land on exact words (13107 and 39321).
    s = reading_of(r, 600.0f, SCD4X_TEMPERATURE_MIN_C, 40.0f);
    HAC_CHECK(s.temperature_c.status == VAL_OK);
    HAC_CHECK_EQ_F32(s.temperature_c.value, -10.0f, 0.0f);
    s = reading_of(r, 600.0f, SCD4X_TEMPERATURE_MAX_C, 40.0f);
    HAC_CHECK(s.temperature_c.status == VAL_OK);
    HAC_CHECK_EQ_F32(s.temperature_c.value, 60.0f, 0.0f);
    s = reading_of(r, 600.0f, SCD4X_TEMPERATURE_MIN_C - 0.01f, 40.0f);
    HAC_CHECK(s.temperature_c.status == VAL_ERROR);
    HAC_CHECK(s.co2_ppm.status == VAL_OK);  // each field judged alone
    s = reading_of(r, 600.0f, SCD4X_TEMPERATURE_MAX_C + 0.01f, 40.0f);
    HAC_CHECK(s.temperature_c.status == VAL_ERROR);

    s = reading_of(r, 600.0f, 21.0f, SCD4X_HUMIDITY_MIN_RH);
    HAC_CHECK(s.humidity_rh.status == VAL_OK);
    s = reading_of(r, 600.0f, 21.0f, SCD4X_HUMIDITY_MAX_RH);
    HAC_CHECK(s.humidity_rh.status == VAL_OK);
    HAC_CHECK_EQ_F32(s.humidity_rh.value, 100.0f, 0.0f);
    rig_done(r);
}

static void an_out_of_range_value_is_an_error_even_during_warm_up(void)
{
    rig_t *r = rig_make(SCD4X_MODE_PERIODIC, true);
    r->air.co2_ppm = 0.0f;
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(read_when_ready(r, &s), HAL_OK);
    HAC_CHECK(s.co2_ppm.status == VAL_ERROR);
    HAC_CHECK(s.temperature_c.status == VAL_WARMUP);
    rig_done(r);
}

// --- ambient pressure, suspend --------------------------------------------------

static void ambient_pressure_reaches_the_sensor_within_its_range(void)
{
    rig_t *r = rig_make(SCD4X_MODE_PERIODIC, true);
    HAC_CHECK_EQ_INT(scd4x_source_set_ambient_pressure(&r->source, 1000.0f), HAL_ERR_INVALID_ARG);
    start_and_warm_up(r);  // measuring: the command is allowed (3.7.5)

    HAC_CHECK_EQ_INT(scd4x_source_set_ambient_pressure(&r->source, 1013.25f), HAL_OK);
    HAC_CHECK_EQ_INT(r->sensor.ambient_pressure_hpa, 1013);
    HAC_CHECK_EQ_INT(scd4x_source_set_ambient_pressure(&r->source, SCD4X_AMBIENT_PRESSURE_MIN_HPA),
                     HAL_OK);
    HAC_CHECK_EQ_INT(r->sensor.ambient_pressure_hpa, 700);
    HAC_CHECK_EQ_INT(scd4x_source_set_ambient_pressure(&r->source, SCD4X_AMBIENT_PRESSURE_MAX_HPA),
                     HAL_OK);
    HAC_CHECK_EQ_INT(r->sensor.ambient_pressure_hpa, 1200);

    const uint32_t transfers = r->bus.transfers;
    HAC_CHECK_EQ_INT(scd4x_source_set_ambient_pressure(&r->source, 699.9f), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(scd4x_source_set_ambient_pressure(&r->source, 1200.1f), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(scd4x_source_set_ambient_pressure(&r->source, NAN), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT((int)(r->bus.transfers - transfers), 0);
    HAC_CHECK_EQ_INT(r->sensor.ambient_pressure_hpa, 1200);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.rejected, 0);
    rig_done(r);
}

static void suspend_stops_periodic_measurement_and_start_resumes_it(void)
{
    rig_t *r = rig_make(SCD4X_MODE_PERIODIC, true);
    start_and_warm_up(r);
    HAC_CHECK_EQ_INT(r->src->suspend(r->src), HAL_OK);
    HAC_CHECK(r->sensor.state == SCD41_MODEL_IDLE);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_NOT_READY);
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);
    HAC_CHECK(r->sensor.state == SCD41_MODEL_PERIODIC);
    HAC_CHECK_EQ_INT(read_when_ready(r, &s), HAL_OK);
    HAC_CHECK(s.co2_ppm.status == VAL_OK);  // the warm-up is counted once, from the first start
    rig_done(r);
}

// --- the HAL glue --------------------------------------------------------------

static void the_hal_sleeps_at_least_as_long_as_asked(void)
{
    adp_clock_virtual_t clock;
    adp_clock_virtual_init(&clock, 1.0);
    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, &clock);
    int owner = 0;
    HAC_CHECK_EQ_INT(
        scd4x_hal_claim(&owner, adp_i2c_fake_port(&bus), adp_clock_virtual_port(&clock)), HAL_OK);

    sensirion_i2c_hal_sleep_usec(1u);
    HAC_CHECK_EQ_U64(clock.now_ms, 1u);
    sensirion_i2c_hal_sleep_usec(1000u);
    HAC_CHECK_EQ_U64(clock.now_ms, 2u);
    sensirion_i2c_hal_sleep_usec(1001u);
    HAC_CHECK_EQ_U64(clock.now_ms, 4u);
    sensirion_i2c_hal_sleep_usec(0u);
    HAC_CHECK_EQ_U64(clock.now_ms, 4u);
    scd4x_hal_release(&owner);
}

static void an_unclaimed_hal_touches_no_bus(void)
{
    uint8_t d[3] = {0};
    HAC_CHECK_EQ_INT(sensirion_i2c_hal_read(ADDR, d, sizeof d), I2C_BUS_ERROR);
    HAC_CHECK_EQ_INT(scd4x_hal_last_transfer().status, HAL_ERR_INVALID_ARG);
    HAC_CHECK(scd4x_hal_last_transfer().was_read);
    HAC_CHECK_EQ_INT(sensirion_i2c_hal_write(ADDR, d, sizeof d), I2C_BUS_ERROR);
    HAC_CHECK(!scd4x_hal_last_transfer().was_read);

    int owner = 0;
    HAC_CHECK_EQ_INT(scd4x_hal_claim(&owner, NULL, NULL), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(sensirion_i2c_hal_select_bus(0u), NO_ERROR);
    HAC_CHECK_EQ_INT(sensirion_i2c_hal_select_bus(1u), NOT_IMPLEMENTED_ERROR);
}

static void the_hal_reports_what_the_port_said(void)
{
    adp_clock_virtual_t clock;
    adp_clock_virtual_init(&clock, 1.0);
    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, &clock);
    int owner = 0;
    HAC_CHECK_EQ_INT(
        scd4x_hal_claim(&owner, adp_i2c_fake_port(&bus), adp_clock_virtual_port(&clock)), HAL_OK);

    uint8_t d[2] = {0x36, 0x82};
    HAC_CHECK_EQ_INT(sensirion_i2c_hal_write(ADDR, d, sizeof d), I2C_NACK_ERROR);  // nothing there
    HAC_CHECK_EQ_INT(scd4x_hal_last_transfer().status, HAL_ERR_NOT_FOUND);
    adp_i2c_fake_hold_scl(&bus, true);
    HAC_CHECK_EQ_INT(sensirion_i2c_hal_read(ADDR, d, sizeof d), I2C_BUS_ERROR);
    HAC_CHECK_EQ_INT(scd4x_hal_last_transfer().status, HAL_ERR_TIMEOUT);
    scd4x_hal_release(&owner);
}

int main(void)
{
    HAC_RUN(init_logs_the_serial_number_and_the_variant);
    HAC_RUN(init_stops_a_sensor_a_reset_left_measuring);
    HAC_RUN(without_the_stop_a_measuring_sensor_refuses_init);
    HAC_RUN(init_waits_out_the_power_up_time);
    HAC_RUN(every_vendor_wait_is_at_least_the_datasheets);
    HAC_RUN(an_absent_sensor_fails_init_as_not_found);
    HAC_RUN(an_scd40_is_refused_single_shot_mode_but_measures_periodically);
    HAC_RUN(only_one_source_can_hold_the_hal);
    HAC_RUN(readings_are_warmup_first_then_ok);
    HAC_RUN(a_sensor_with_nothing_new_is_not_ready);
    HAC_RUN(a_read_header_refused_after_the_command_is_not_yet_not_gone);
    HAC_RUN(a_bad_crc_is_an_io_error_and_every_field_an_error);
    HAC_RUN(a_bad_crc_on_the_measurement_itself_is_an_io_error);
    HAC_RUN(a_byte_refused_mid_transfer_is_an_io_error);
    HAC_RUN(a_sensor_that_stops_answering_is_not_found_and_logged_once);
    HAC_RUN(a_stalled_bus_is_a_timeout);
    HAC_RUN(a_single_shot_is_not_disturbed_while_it_measures);
    HAC_RUN(a_slow_single_shot_is_busy_within_the_grace);
    HAC_RUN(a_single_shot_refused_past_the_grace_is_not_found);
    HAC_RUN(values_outside_the_datasheets_range_are_errors);
    HAC_RUN(an_out_of_range_value_is_an_error_even_during_warm_up);
    HAC_RUN(ambient_pressure_reaches_the_sensor_within_its_range);
    HAC_RUN(suspend_stops_periodic_measurement_and_start_resumes_it);
    HAC_RUN(the_hal_sleeps_at_least_as_long_as_asked);
    HAC_RUN(an_unclaimed_hal_touches_no_bus);
    HAC_RUN(the_hal_reports_what_the_port_said);
    return hac_test_summary();
}
