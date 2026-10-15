// SPDX-License-Identifier: Apache-2.0
//
// bme68x_source -- Bosch's own Sensor API behind sensor_source_t -- over the
// fake bus and the BME680 model, at its boundaries: every way the bus and
// the sensor can fail, the operating range, the gas status bits and burn-in,
// the self-heating offset, the timing, a sensor left measuring, and the HAL
// glue under it all. The happy path is the contract's
// (tests/contract/test_port_sensor_contract.c).
//
// The model inverts the same compensation Bosch's code applies
// (bme680_model.h), so a reading coming back as the value the model was
// given proves the registers, framing and bits between them -- not Bosch's
// arithmetic.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_i2c_fake.h"
#include "bme68x.h"
#include "bme68x_hal.h"
#include "bme68x_limits.h"
#include "bme68x_source.h"
#include "hac_test.h"
#include "i2c_models/bme680_model.h"

#define ADDR 0x77u
#define SOURCE_ID 4u

// What init() computes from Bosch's estimate for the oversampling and
// heater of bme68x_limits.h: 42.59 ms of conversions and 100 ms of heating.
#define MEASUREMENT_MS 143u

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

static void reading_from_air(void *ctx, uint64_t at_ms, bme680_model_reading_t *out)
{
    (void)at_ms;
    *out = *(const bme680_model_reading_t *)ctx;
}

typedef struct {
    adp_clock_virtual_t clock;
    adp_i2c_fake_t bus;
    bme680_model_t sensor;
    bme680_model_reading_t air;  // what the die measures
    capture_log_t log;
    bme68x_source_t source;
    sensor_source_t *src;
} rig_t;

// Static: the rig is large, and every test makes one.
static rig_t g_rig;

static rig_t *rig_make(bool attach)
{
    rig_t *r = &g_rig;
    memset(r, 0, sizeof *r);
    adp_clock_virtual_init(&r->clock, 1.0);
    adp_i2c_fake_init(&r->bus, &r->clock);
    bme680_model_init(&r->sensor, 0u);
    r->air = (bme680_model_reading_t){.temperature_c = 24.3f,
                                      .humidity_rh = 51.7f,
                                      .pressure_hpa = 998.4f,
                                      .gas_resistance_ohm = 87000.0f};
    bme680_model_set_reading(&r->sensor, reading_from_air, &r->air);
    if (attach) {
        (void)adp_i2c_fake_attach(&r->bus, ADDR, bme680_model_port(&r->sensor));
    }
    r->log.port.write = capture_write;
    r->log.port.impl = &r->log;

    const bme68x_source_config_t cfg = {
        .bus = adp_i2c_fake_port(&r->bus),
        .clock = adp_clock_virtual_port(&r->clock),
        .log = &r->log.port,
        .address = ADDR,
        .id = SOURCE_ID,
    };
    (void)bme68x_source_setup(&r->source, &cfg);
    r->src = &r->source.port;
    return r;
}

static void advance(rig_t *r, uint64_t ms)
{
    adp_clock_virtual_advance(&r->clock, ms);
}

// Enough "not yet"s to cover the grace at the retry rate.
#define READ_ATTEMPTS (HAC_BME68X_MEASUREMENT_GRACE_MS / HAC_BME68X_NOT_READY_RETRY_MS + 4u)

// Waits what ready_in_ms() asks and reads, until the source answers
// something other than "not yet".
static hal_status_t read_when_ready(rig_t *r, sensor_sample_t *out)
{
    hal_status_t status = HAL_ERR_NOT_READY;
    for (unsigned attempt = 0; attempt < READ_ATTEMPTS && status == HAL_ERR_NOT_READY; attempt++) {
        advance(r, r->src->ready_in_ms(r->src));
        status = r->src->read(r->src, out);
    }
    return status;
}

// One measurement, start to reading.
static hal_status_t measure(rig_t *r, sensor_sample_t *out)
{
    const hal_status_t status = r->src->start(r->src);
    if (status != HAL_OK) {
        return status;
    }
    return read_when_ready(r, out);
}

// Initialised and past the gas burn-in, so statuses say VAL_OK or VAL_ERROR.
static void init_and_burn_in(rig_t *r)
{
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    sensor_sample_t discard;
    HAC_CHECK_EQ_INT(measure(r, &discard), HAL_OK);
    advance(r, HAC_BME68X_GAS_BURN_IN_MS);
}

static bool all_four_error(const sensor_sample_t *s)
{
    return s->temperature_c.status == VAL_ERROR && s->humidity_rh.status == VAL_ERROR &&
           s->pressure_hpa.status == VAL_ERROR && s->gas_resistance_ohm.status == VAL_ERROR;
}

static bool nothing_outside_caps(const sensor_sample_t *s)
{
    return s->co2_ppm.status == VAL_UNAVAILABLE && s->iaq.status == VAL_UNAVAILABLE &&
           s->voc_index.status == VAL_UNAVAILABLE;
}

// --- init ----------------------------------------------------------------------

static void it_claims_no_iaq_without_bsec(void)
{
    rig_t *r = rig_make(true);
    HAC_CHECK(strcmp(r->src->name, "bme68x") == 0);
    HAC_CHECK_EQ_INT(r->src->id, SOURCE_ID);
    HAC_CHECK_EQ_INT(r->src->caps, SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY | SENS_CAP_PRESSURE |
                                       SENS_CAP_GAS_RES);
    HAC_CHECK((r->src->caps & (SENS_CAP_IAQ | SENS_CAP_VOC | SENS_CAP_CO2)) == 0);
}

static void init_logs_the_chip_and_its_variant(void)
{
    rig_t *r = rig_make(true);
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(log_count(&r->log, "bme68x: sensor ready"), 1);
    HAC_CHECK_EQ_INT(log_count(&r->log, "address=119"), 1);
    HAC_CHECK_EQ_INT(log_count(&r->log, "chip_id=0x61"), 1);
    HAC_CHECK_EQ_INT(log_count(&r->log, "variant=BME680 variant_id=0x00"), 1);
    HAC_CHECK_EQ_INT(log_count(&r->log, "measurement_ms=143"), 1);
    HAC_CHECK_EQ_INT(r->source.measurement_ms, MEASUREMENT_MS);
    r->src->deinit(r->src);
}

static void a_bme688_is_named_as_one(void)
{
    rig_t *r = rig_make(true);
    r->sensor.variant_id = BME680_MODEL_VARIANT_BME688;  // identity only (bme680_model.h)
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(log_count(&r->log, "variant=BME688 variant_id=0x01"), 1);
    HAC_CHECK(strcmp(bme68x_variant_name(0x02u), "unknown") == 0);
}

// The byte order and nibble packing of every parameter, through Bosch's own
// reader: what it holds is the published set the model was given.
static void init_reads_the_calibration_back_through_bosch(void)
{
    rig_t *r = rig_make(true);
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    const struct bme68x_calib_data *k = &r->source.dev.calib;
    const bme680_model_calibration_t *p = &BME680_MODEL_PUBLISHED_CALIBRATION;
    HAC_CHECK_EQ_INT(k->par_t1, p->par_t1);
    HAC_CHECK_EQ_INT(k->par_t2, p->par_t2);
    HAC_CHECK_EQ_INT(k->par_t3, p->par_t3);
    HAC_CHECK_EQ_INT(k->par_p1, p->par_p1);
    HAC_CHECK_EQ_INT(k->par_p2, p->par_p2);
    HAC_CHECK_EQ_INT(k->par_p3, p->par_p3);
    HAC_CHECK_EQ_INT(k->par_p4, p->par_p4);
    HAC_CHECK_EQ_INT(k->par_p5, p->par_p5);
    HAC_CHECK_EQ_INT(k->par_p6, p->par_p6);
    HAC_CHECK_EQ_INT(k->par_p7, p->par_p7);
    HAC_CHECK_EQ_INT(k->par_p8, p->par_p8);
    HAC_CHECK_EQ_INT(k->par_p9, p->par_p9);
    HAC_CHECK_EQ_INT(k->par_p10, p->par_p10);
    HAC_CHECK_EQ_INT(k->par_h1, p->par_h1);
    HAC_CHECK_EQ_INT(k->par_h2, p->par_h2);
    HAC_CHECK_EQ_INT(k->par_h3, p->par_h3);
    HAC_CHECK_EQ_INT(k->par_h4, p->par_h4);
    HAC_CHECK_EQ_INT(k->par_h5, p->par_h5);
    HAC_CHECK_EQ_INT(k->par_h6, p->par_h6);
    HAC_CHECK_EQ_INT(k->par_h7, p->par_h7);
    HAC_CHECK_EQ_INT(k->par_gh1, p->par_g1);
    HAC_CHECK_EQ_INT(k->par_gh2, p->par_g2);
    HAC_CHECK_EQ_INT(k->par_gh3, p->par_g3);
    HAC_CHECK_EQ_INT(k->res_heat_range, p->res_heat_range);
    HAC_CHECK_EQ_INT(k->res_heat_val, p->res_heat_val);
    HAC_CHECK_EQ_INT(k->range_sw_err, p->range_switching_error);

    // And a negative range switching error, which is a signed 4-bit field.
    r->sensor.calibration.range_switching_error = -5;
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(r->source.dev.calib.range_sw_err, -5);
}

static void init_configures_the_sensor_as_the_datasheet_shows(void)
{
    rig_t *r = rig_make(true);
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    const bme680_model_t *m = &r->sensor;
    HAC_CHECK_EQ_INT(bme680_model_peek(m, 0x72u) & 0x07u, 0x01);         // osrs_h x1
    HAC_CHECK_EQ_INT(bme680_model_peek(m, 0x74u), (2 << 5) | (5 << 2));  // osrs_t x2, osrs_p x16
    HAC_CHECK_EQ_INT(bme680_model_peek(m, 0x75u), 0x00);                 // filter off
    HAC_CHECK_EQ_INT(bme680_model_peek(m, 0x64u), 0x59);                 // 100 ms, as 3.2.1 has it
    HAC_CHECK_EQ_INT(bme680_model_peek(m, 0x71u), 0x10);                 // run_gas, profile 0
    HAC_CHECK_EQ_INT(bme680_model_peek(m, 0x70u), 0x00);                 // heater on
    // The heater code against the datasheet's own formula (3.3.5) -- the one
    // place Bosch's arithmetic meets an independent transcription here.
    HAC_CHECK_EQ_INT(bme680_model_peek(m, 0x5Au),
                     bme680_model_res_heat(&m->calibration, HAC_BME68X_HEATER_TEMPERATURE_C,
                                           HAC_BME68X_HEATER_AMBIENT_C));
}

static void init_resets_a_sensor_a_software_reset_left_measuring(void)
{
    rig_t *r = rig_make(true);
    // A previous firmware's configuration, measuring when the CPU reset.
    advance(r, BME680_MODEL_START_UP_MS);
    port_i2c_t *bus = adp_i2c_fake_port(&r->bus);
    const uint8_t theirs[] = {0x72u, 0x05u, 0x75u, 0x10u, 0x71u, 0x10u, 0x64u, 0xFFu, 0x74u, 0xB5u};
    HAC_CHECK_EQ_INT(bus->write(bus, ADDR, theirs, sizeof theirs, 20u), HAL_OK);
    HAC_CHECK(r->sensor.measuring);

    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.soft_resets, 1);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.ignored_writes, 0);
    HAC_CHECK_EQ_INT(bme680_model_peek(&r->sensor, 0x72u), 0x01);
    HAC_CHECK_EQ_INT(bme680_model_peek(&r->sensor, 0x75u), 0x00);
    HAC_CHECK_EQ_INT(bme680_model_peek(&r->sensor, 0x64u), 0x59);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(measure(r, &s), HAL_OK);
}

static void init_waits_out_the_start_up_time(void)
{
    // The supply has only just risen: the model refuses its address for
    // 2 ms. init() must not be refused.
    rig_t *r = rig_make(true);
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.nacked_while_busy, 0);
}

static void every_wait_is_long_enough_and_no_write_is_wasted(void)
{
    rig_t *r = rig_make(true);
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    sensor_sample_t s;
    for (int i = 0; i < 5; i++) {
        HAC_CHECK_EQ_INT(r->src->ready_in_ms(r->src), 0);
        HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);
        HAC_CHECK_EQ_INT(r->src->ready_in_ms(r->src), MEASUREMENT_MS);
        HAC_CHECK_EQ_INT(read_when_ready(r, &s), HAL_OK);
        advance(r, 3000u);
    }
    HAC_CHECK_EQ_INT((int)r->sensor.stats.completed, 5);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.early_reads, 0);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.ignored_writes, 0);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.nacked_while_busy, 0);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.read_only_writes, 0);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.soft_resets, 1);  // init's, and no other
}

static void an_absent_sensor_fails_init_as_not_found(void)
{
    rig_t *r = rig_make(false);
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_ERR_NOT_FOUND);
    HAC_CHECK_EQ_INT(log_count(&r->log, "sensor not usable step=init status=not_found"), 1);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_NOT_READY);
    HAC_CHECK_EQ_INT(r->src->suspend(r->src), HAL_OK);
}

static void something_else_at_the_address_is_unsupported(void)
{
    rig_t *r = rig_make(true);
    r->sensor.chip_id = 0x58u;  // a BMP280, which can sit at 0x77 too (datasheet 6.2)
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_ERR_UNSUPPORTED);
    HAC_CHECK_EQ_INT(log_count(&r->log, "not a BME68x chip_id=0x58"), 1);
}

// The steps init() names when it fails, in the order it takes them.
static const char *const INIT_STEPS[] = {"step=init ", "step=reset ", "step=set_conf ",
                                         "step=set_heatr_conf "};
#define INIT_STEP_COUNT (sizeof INIT_STEPS / sizeof INIT_STEPS[0])

static int step_logged(const capture_log_t *log)
{
    for (size_t i = 0; i < INIT_STEP_COUNT; i++) {
        if (log_count(log, INIT_STEPS[i]) > 0) {
            return (int)i;
        }
    }
    return -1;
}

// Every one of init()'s calls on the sensor refused in turn: each fails
// init() and names its step, and the steps come in order -- without this
// test knowing how many calls Bosch makes in each.
static void every_call_of_init_refused_fails_it_and_names_its_step(void)
{
    rig_t *r = rig_make(true);
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    const uint32_t calls = r->sensor.stats.calls;
    HAC_CHECK(calls > 10u);

    int last_step = 0;
    bool seen[INIT_STEP_COUNT] = {false};
    for (uint32_t n = 1u; n <= calls; n++) {
        r = rig_make(true);
        r->sensor.faults.nack_call = n;
        const hal_status_t status = r->src->init(r->src);
        if (n == 1u) {
            // The soft reset: bme68x_init() ignores its result, and a sensor
            // that was not measuring needs nothing it would have done.
            HAC_CHECK_EQ_INT(status, HAL_OK);
            continue;
        }
        HAC_CHECK_EQ_INT(status, HAL_ERR_NOT_FOUND);
        HAC_CHECK(!r->source.initialized);
        const int step = step_logged(&r->log);
        HAC_CHECK(step >= last_step);
        if (step >= 0) {
            seen[step] = true;
            last_step = step;
        }
    }
    for (size_t i = 0; i < INIT_STEP_COUNT; i++) {
        HAC_CHECK(seen[i]);
    }
}

static void a_reset_that_did_not_take_is_refused(void)
{
    // A sensor left measuring for ever, whose soft reset is refused: the
    // mode check finds it still measuring, and init() stops there rather
    // than enter Bosch's unbounded wait for sleep.
    rig_t *r = rig_make(true);
    advance(r, BME680_MODEL_START_UP_MS);
    r->sensor.faults.overrun_ms = BME680_MODEL_NEVER;
    port_i2c_t *bus = adp_i2c_fake_port(&r->bus);
    const uint8_t forced[] = {0x74u, 0x01u};
    HAC_CHECK_EQ_INT(bus->write(bus, ADDR, forced, sizeof forced, 20u), HAL_OK);
    r->sensor.faults.nack_data = 1u;  // the 0xB6 to the reset register
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_ERR_IO);
    HAC_CHECK_EQ_INT(log_count(&r->log, "sensor not usable step=reset status=io"), 1);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.soft_resets, 0);
}

// --- readings --------------------------------------------------------------------

static void a_reading_carries_the_offset_and_the_moved_humidity(void)
{
    rig_t *r = rig_make(true);
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_NOT_READY);  // before start
    HAC_CHECK_EQ_INT(measure(r, &s), HAL_OK);

    const float t_air = 24.3f - HAC_BME68X_SELF_HEATING_C;
    HAC_CHECK(s.temperature_c.status == VAL_OK);
    HAC_CHECK_EQ_F32(s.temperature_c.value, t_air, 0.01f);
    HAC_CHECK(s.humidity_rh.status == VAL_OK);
    HAC_CHECK_EQ_F32(s.humidity_rh.value, bme68x_relative_humidity_at(51.7f, 24.3f, t_air), 0.02f);
    HAC_CHECK(s.humidity_rh.value > 51.7f);  // cooler air, the same water: wetter
    HAC_CHECK(s.pressure_hpa.status == VAL_OK);
    HAC_CHECK_EQ_F32(s.pressure_hpa.value, 998.4f, 0.01f);
    HAC_CHECK(s.gas_resistance_ohm.status == VAL_WARMUP);  // burning in
    HAC_CHECK_EQ_INT(s.temperature_c.source_id, SOURCE_ID);
    HAC_CHECK_EQ_INT(s.gas_resistance_ohm.source_id, SOURCE_ID);
    HAC_CHECK_EQ_U64(s.temperature_c.ts_ms, r->clock.now_ms);
    HAC_CHECK(nothing_outside_caps(&s));

    advance(r, HAC_BME68X_GAS_BURN_IN_MS);
    HAC_CHECK_EQ_INT(measure(r, &s), HAL_OK);
    HAC_CHECK(s.gas_resistance_ohm.status == VAL_OK);
    HAC_CHECK_EQ_F32(s.gas_resistance_ohm.value, 87000.0f, 87.0f);
}

static void the_burn_in_ends_exactly_when_it_says(void)
{
    rig_t *r = rig_make(true);
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    const uint64_t first_start = r->clock.now_ms;
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(measure(r, &s), HAL_OK);
    const uint64_t until = first_start + HAC_BME68X_GAS_BURN_IN_MS;

    // A reading collected 1 ms before the end, and one collected at it.
    advance(r, until - 1u - MEASUREMENT_MS - r->clock.now_ms);
    HAC_CHECK_EQ_INT(measure(r, &s), HAL_OK);
    HAC_CHECK_EQ_U64(r->clock.now_ms, until - 1u);
    HAC_CHECK(s.gas_resistance_ohm.status == VAL_WARMUP);
    HAC_CHECK(s.temperature_c.status == VAL_OK);  // only the gas burns in
    advance(r, 1u);
    HAC_CHECK_EQ_INT(measure(r, &s), HAL_OK);
    HAC_CHECK_EQ_U64(r->clock.now_ms, until + MEASUREMENT_MS);
    HAC_CHECK(s.gas_resistance_ohm.status == VAL_OK);
}

static void before_it_is_done_the_bus_is_left_alone(void)
{
    rig_t *r = rig_make(true);
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);
    const uint32_t transfers = r->bus.transfers;
    sensor_sample_t s;
    memset(&s, 0xA5, sizeof s);
    advance(r, MEASUREMENT_MS - 1u);
    HAC_CHECK_EQ_INT(r->src->ready_in_ms(r->src), 1);
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_NOT_READY);
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_ERR_BUSY);
    HAC_CHECK_EQ_INT(r->src->suspend(r->src), HAL_ERR_BUSY);
    HAC_CHECK_EQ_INT((int)(r->bus.transfers - transfers), 0);
    uint8_t untouched[sizeof s];
    memset(untouched, 0xA5, sizeof untouched);
    HAC_CHECK(memcmp(&s, untouched, sizeof s) == 0);  // out is left alone

    advance(r, 1u);
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_OK);
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_NOT_READY);  // one start, one reading
    HAC_CHECK_EQ_INT(r->src->suspend(r->src), HAL_OK);
}

static void suspend_drops_a_reading_not_collected(void)
{
    rig_t *r = rig_make(true);
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);
    advance(r, MEASUREMENT_MS);
    HAC_CHECK_EQ_INT(r->src->suspend(r->src), HAL_OK);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_NOT_READY);
    HAC_CHECK_EQ_INT(r->src->ready_in_ms(r->src), 0);
    HAC_CHECK_EQ_INT(measure(r, &s), HAL_OK);  // and starts again as ever
}

// --- the gas status bits -------------------------------------------------------

static void gas_not_marked_valid_is_not_a_reading(void)
{
    rig_t *r = rig_make(true);
    init_and_burn_in(r);
    r->sensor.faults.gas_invalid = true;
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(measure(r, &s), HAL_OK);
    HAC_CHECK(s.gas_resistance_ohm.status == VAL_ERROR);
    HAC_CHECK(s.temperature_c.status == VAL_OK);
    HAC_CHECK(s.pressure_hpa.status == VAL_OK);
    HAC_CHECK_EQ_INT(measure(r, &s), HAL_OK);
    HAC_CHECK_EQ_INT(log_count(&r->log, "gas not measured gas_valid=false heat_stab=true"), 1);

    r->sensor.faults.gas_invalid = false;
    HAC_CHECK_EQ_INT(measure(r, &s), HAL_OK);
    HAC_CHECK(s.gas_resistance_ohm.status == VAL_OK);
    r->sensor.faults.heater_unstable = true;
    HAC_CHECK_EQ_INT(measure(r, &s), HAL_OK);
    HAC_CHECK(s.gas_resistance_ohm.status == VAL_ERROR);
    HAC_CHECK_EQ_INT(log_count(&r->log, "gas not measured gas_valid=true heat_stab=false"), 1);
}

static void an_invalid_gas_value_is_an_error_even_during_burn_in(void)
{
    rig_t *r = rig_make(true);
    r->sensor.faults.heater_unstable = true;
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(measure(r, &s), HAL_OK);
    HAC_CHECK(s.gas_resistance_ohm.status == VAL_ERROR);
}

// --- ranges --------------------------------------------------------------------

static sensor_sample_t reading_of(rig_t *r, float t, float rh, float p)
{
    r->air.temperature_c = t;
    r->air.humidity_rh = rh;
    r->air.pressure_hpa = p;
    sensor_sample_t s;
    memset(&s, 0, sizeof s);
    HAC_CHECK_EQ_INT(measure(r, &s), HAL_OK);
    return s;
}

static void the_operating_range_bounds_temperature_and_takes_the_rest_with_it(void)
{
    rig_t *r = rig_make(true);
    init_and_burn_in(r);
    // The model's 17-bit temperature lands within 0.003 C of what it is
    // asked for, so 0.01 C either side of a limit is on that side.
    sensor_sample_t s = reading_of(r, HAC_BME68X_TEMPERATURE_MAX_C - 0.01f, 40.0f, 1000.0f);
    HAC_CHECK(s.temperature_c.status == VAL_OK);
    s = reading_of(r, HAC_BME68X_TEMPERATURE_MAX_C + 0.01f, 40.0f, 1000.0f);
    HAC_CHECK(s.temperature_c.status == VAL_ERROR);
    HAC_CHECK(s.humidity_rh.status == VAL_ERROR);  // compensated with it
    HAC_CHECK(s.pressure_hpa.status == VAL_ERROR);
    HAC_CHECK(s.gas_resistance_ohm.status == VAL_OK);  // not: judged alone
    s = reading_of(r, HAC_BME68X_TEMPERATURE_MIN_C + 0.01f, 40.0f, 1000.0f);
    HAC_CHECK(s.temperature_c.status == VAL_OK);
    HAC_CHECK_EQ_F32(s.temperature_c.value, -40.0f - HAC_BME68X_SELF_HEATING_C, 0.02f);
    s = reading_of(r, HAC_BME68X_TEMPERATURE_MIN_C - 0.01f, 40.0f, 1000.0f);
    HAC_CHECK(s.temperature_c.status == VAL_ERROR);
    HAC_CHECK(s.pressure_hpa.status == VAL_ERROR);
}

static void the_operating_range_bounds_pressure_alone(void)
{
    rig_t *r = rig_make(true);
    init_and_burn_in(r);
    sensor_sample_t s = reading_of(r, 21.0f, 40.0f, HAC_BME68X_PRESSURE_MIN_HPA + 0.01f);
    HAC_CHECK(s.pressure_hpa.status == VAL_OK);
    s = reading_of(r, 21.0f, 40.0f, HAC_BME68X_PRESSURE_MIN_HPA - 0.01f);
    HAC_CHECK(s.pressure_hpa.status == VAL_ERROR);
    HAC_CHECK(s.temperature_c.status == VAL_OK);
    HAC_CHECK(s.humidity_rh.status == VAL_OK);
    s = reading_of(r, 21.0f, 40.0f, HAC_BME68X_PRESSURE_MAX_HPA - 0.01f);
    HAC_CHECK(s.pressure_hpa.status == VAL_OK);
    s = reading_of(r, 21.0f, 40.0f, HAC_BME68X_PRESSURE_MAX_HPA + 0.01f);
    HAC_CHECK(s.pressure_hpa.status == VAL_ERROR);
}

static void humidity_at_its_limits_is_a_reading(void)
{
    rig_t *r = rig_make(true);
    init_and_burn_in(r);
    sensor_sample_t s = reading_of(r, 21.0f, 0.0f, 1000.0f);
    HAC_CHECK(s.humidity_rh.status == VAL_OK);
    HAC_CHECK_EQ_F32(s.humidity_rh.value, 0.0f, 0.01f);
    s = reading_of(r, 21.0f, 100.0f, 1000.0f);  // and moved past 100, clamped
    HAC_CHECK(s.humidity_rh.status == VAL_OK);
    HAC_CHECK_EQ_F32(s.humidity_rh.value, 100.0f, 0.0f);
}

static void humidity_moves_with_the_temperature_as_magnus_says(void)
{
    // 50 %RH at 25 C is 50.90 %RH at 24.7 C: the ratio of the saturation
    // pressures, 31.67 and 31.11 hPa by the WMO's Magnus coefficients.
    HAC_CHECK_EQ_F32(bme68x_relative_humidity_at(50.0f, 25.0f, 24.7f), 50.903f, 0.002f);
    HAC_CHECK_EQ_F32(bme68x_relative_humidity_at(50.0f, 24.7f, 25.0f), 49.113f, 0.002f);
    HAC_CHECK_EQ_F32(bme68x_relative_humidity_at(42.0f, 18.0f, 18.0f), 42.0f, 0.0f);
    HAC_CHECK_EQ_F32(bme68x_relative_humidity_at(99.0f, 30.0f, 20.0f), 100.0f, 0.0f);
    HAC_CHECK_EQ_F32(bme68x_relative_humidity_at(0.0f, 30.0f, 20.0f), 0.0f, 0.0f);
}

// --- failures ------------------------------------------------------------------

static void a_byte_refused_mid_read_is_an_io_error(void)
{
    rig_t *r = rig_make(true);
    init_and_burn_in(r);
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);
    advance(r, MEASUREMENT_MS);
    r->sensor.faults.nack_data = 1u;  // the register address of the data read
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_IO);
    HAC_CHECK(all_four_error(&s));
    HAC_CHECK_EQ_U64(s.pressure_hpa.ts_ms, r->clock.now_ms);
    HAC_CHECK_EQ_INT(s.gas_resistance_ohm.source_id, SOURCE_ID);
    HAC_CHECK(nothing_outside_caps(&s));
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_NOT_READY);  // abandoned
    HAC_CHECK_EQ_INT(measure(r, &s), HAL_OK);                       // and the next one works
}

static void a_sensor_that_stops_answering_is_not_found_and_logged_once(void)
{
    rig_t *r = rig_make(true);
    init_and_burn_in(r);
    sensor_sample_t s;
    for (int i = 0; i < 2; i++) {
        HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);
        advance(r, MEASUREMENT_MS);
        r->sensor.faults.nack_address = 1u;
        HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_NOT_FOUND);
        HAC_CHECK(all_four_error(&s));
    }
    HAC_CHECK_EQ_INT(log_count(&r->log, "read failed step=get_data status=not_found"), 1);

    r->sensor.faults.nack_address = 1u;
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_ERR_NOT_FOUND);  // start says so too
    HAC_CHECK_EQ_INT(measure(r, &s), HAL_OK);
    HAC_CHECK_EQ_INT(log_count(&r->log, "reading again"), 1);
}

static void a_stalled_bus_is_a_timeout(void)
{
    rig_t *r = rig_make(true);
    init_and_burn_in(r);
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);
    advance(r, MEASUREMENT_MS);
    adp_i2c_fake_hold_scl(&r->bus, true);
    const uint64_t before = r->clock.now_ms;
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_TIMEOUT);
    HAC_CHECK(all_four_error(&s));
    HAC_CHECK_EQ_U64(r->clock.now_ms - before, HAC_BME68X_TRANSFER_TIMEOUT_MS);  // one deadline
    adp_i2c_fake_hold_scl(&r->bus, false);
}

// --- a measurement that takes longer than Bosch says --------------------------

static void a_little_slow_is_absorbed_by_bosch_polling(void)
{
    // Bosch's get_data() reads five times, 10 ms apart, before it says "no
    // new data": a sensor 30 ms late is caught on the fourth.
    rig_t *r = rig_make(true);
    r->sensor.faults.overrun_ms = 30u;
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);
    advance(r, MEASUREMENT_MS);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_OK);
    HAC_CHECK_EQ_INT((int)r->sensor.stats.early_reads, 3);
}

static void a_late_measurement_is_not_yet_within_the_grace(void)
{
    rig_t *r = rig_make(true);
    r->sensor.faults.overrun_ms = 300u;
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);
    advance(r, MEASUREMENT_MS);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(r->src->read(r->src, &s), HAL_ERR_NOT_READY);
    HAC_CHECK_EQ_INT(r->src->ready_in_ms(r->src), HAC_BME68X_NOT_READY_RETRY_MS);
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_ERR_BUSY);  // still measuring, the mode says
    HAC_CHECK_EQ_INT(read_when_ready(r, &s), HAL_OK);
    HAC_CHECK_EQ_INT(log_count(&r->log, "read failed"), 0);
}

static void a_measurement_that_never_ends_is_a_timeout_not_a_hang(void)
{
    rig_t *r = rig_make(true);
    r->sensor.faults.overrun_ms = BME680_MODEL_NEVER;
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    const uint64_t started = r->clock.now_ms;
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(read_when_ready(r, &s), HAL_ERR_TIMEOUT);
    HAC_CHECK(all_four_error(&s));
    HAC_CHECK(r->clock.now_ms >= started + MEASUREMENT_MS + HAC_BME68X_MEASUREMENT_GRACE_MS);
    HAC_CHECK_EQ_INT(log_count(&r->log, "read failed step=measurement_overdue status=timeout"), 1);
    // Bosch's set_op_mode() would wait for sleep forever; start() does not
    // call it while the sensor says it is measuring.
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_ERR_BUSY);
    // A re-init's soft reset is the way out.
    r->sensor.faults.overrun_ms = 0u;
    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(measure(r, &s), HAL_OK);
}

// --- two of them ----------------------------------------------------------------

static void two_sensors_share_a_bus_without_sharing_state(void)
{
    // Bosch's API takes a context, so nothing here is global (bme68x_hal.h).
    rig_t *r = rig_make(true);
    static bme680_model_t second_model;
    bme680_model_init(&second_model, 0u);
    static bme680_model_reading_t second_air = {.temperature_c = 30.0f,
                                                .humidity_rh = 20.0f,
                                                .pressure_hpa = 950.0f,
                                                .gas_resistance_ohm = 5000.0f};
    bme680_model_set_reading(&second_model, reading_from_air, &second_air);
    HAC_CHECK(adp_i2c_fake_attach(&r->bus, 0x76u, bme680_model_port(&second_model)));
    static bme68x_source_t second;
    bme68x_source_config_t cfg = r->source.cfg;
    cfg.address = 0x76u;
    cfg.id = SOURCE_ID + 1u;
    HAC_CHECK_EQ_INT(bme68x_source_setup(&second, &cfg), HAL_OK);

    HAC_CHECK_EQ_INT(r->src->init(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(second.port.init(&second.port), HAL_OK);
    HAC_CHECK_EQ_INT(r->src->start(r->src), HAL_OK);
    HAC_CHECK_EQ_INT(second.port.start(&second.port), HAL_OK);
    advance(r, MEASUREMENT_MS);
    sensor_sample_t a;
    sensor_sample_t b;
    HAC_CHECK_EQ_INT(r->src->read(r->src, &a), HAL_OK);
    HAC_CHECK_EQ_INT(second.port.read(&second.port, &b), HAL_OK);
    HAC_CHECK_EQ_F32(a.pressure_hpa.value, 998.4f, 0.01f);
    HAC_CHECK_EQ_F32(b.pressure_hpa.value, 950.0f, 0.01f);
    HAC_CHECK_EQ_INT(b.pressure_hpa.source_id, SOURCE_ID + 1u);
}

// --- set-up and the HAL glue ----------------------------------------------------

static void setup_refuses_a_missing_bus_or_clock(void)
{
    static bme68x_source_t s;
    bme68x_source_config_t cfg = {.bus = NULL, .clock = NULL, .address = ADDR};
    HAC_CHECK_EQ_INT(bme68x_source_setup(&s, &cfg), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(bme68x_source_setup(&s, NULL), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(bme68x_source_setup(NULL, &cfg), HAL_ERR_INVALID_ARG);
}

static void the_hal_sleeps_at_least_as_long_as_asked(void)
{
    adp_clock_virtual_t clock;
    adp_clock_virtual_init(&clock, 1.0);
    bme68x_hal_t hal = {.bus = NULL, .clock = adp_clock_virtual_port(&clock), .address = ADDR};

    bme68x_hal_delay_us(1u, &hal);
    HAC_CHECK_EQ_U64(clock.now_ms, 1u);
    bme68x_hal_delay_us(1000u, &hal);
    HAC_CHECK_EQ_U64(clock.now_ms, 2u);
    bme68x_hal_delay_us(1001u, &hal);
    HAC_CHECK_EQ_U64(clock.now_ms, 4u);
    bme68x_hal_delay_us(0u, &hal);
    HAC_CHECK_EQ_U64(clock.now_ms, 4u);
    bme68x_hal_delay_us(BME68X_PERIOD_POLL, &hal);  // 10 ms exactly
    HAC_CHECK_EQ_U64(clock.now_ms, 14u);
    bme68x_hal_delay_us(5u, NULL);  // nothing to wait with: no crash, no wait
    HAC_CHECK_EQ_U64(clock.now_ms, 14u);
}

static void the_hal_refuses_what_it_cannot_send(void)
{
    adp_clock_virtual_t clock;
    adp_clock_virtual_init(&clock, 1.0);
    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, &clock);
    bme68x_hal_t hal = {.bus = adp_i2c_fake_port(&bus), .clock = NULL, .address = ADDR};
    uint8_t d[BME68X_LEN_INTERLEAVE_BUFF] = {0};

    HAC_CHECK_EQ_INT(bme68x_hal_read(0xD0u, d, 1u, NULL), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(bme68x_hal_write(0x72u, d, 1u, NULL), HAL_ERR_INVALID_ARG);
    // Bosch's widest write is the first address and 19 bytes; one more is a bug.
    HAC_CHECK_EQ_INT(bme68x_hal_write(0x72u, d, BME68X_LEN_INTERLEAVE_BUFF, &hal),
                     HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(bus.transfers, 0);
    HAC_CHECK_EQ_INT(bme68x_hal_write(0x72u, d, BME68X_LEN_INTERLEAVE_BUFF - 1u, &hal),
                     HAL_ERR_NOT_FOUND);  // sent, and nothing there
    HAC_CHECK_EQ_INT(bme68x_hal_read(0xD0u, d, 1u, &hal), HAL_ERR_NOT_FOUND);
}

static void the_hal_status_is_what_the_port_said(void)
{
    struct bme68x_dev dev;
    memset(&dev, 0, sizeof dev);
    dev.intf_rslt = (BME68X_INTF_RET_TYPE)HAL_ERR_NOT_FOUND;
    HAC_CHECK_EQ_INT(bme68x_hal_status(&dev), HAL_ERR_NOT_FOUND);
    dev.intf_rslt = (BME68X_INTF_RET_TYPE)HAL_ERR_TIMEOUT;
    HAC_CHECK_EQ_INT(bme68x_hal_status(&dev), HAL_ERR_TIMEOUT);
    dev.intf_rslt = 0;
    HAC_CHECK_EQ_INT(bme68x_hal_status(&dev), HAL_ERR_IO);  // no failure to report
    dev.intf_rslt = 99;
    HAC_CHECK_EQ_INT(bme68x_hal_status(&dev), HAL_ERR_IO);
    dev.intf_rslt = -1;
    HAC_CHECK_EQ_INT(bme68x_hal_status(&dev), HAL_ERR_IO);
}

int main(void)
{
    HAC_RUN(it_claims_no_iaq_without_bsec);
    HAC_RUN(init_logs_the_chip_and_its_variant);
    HAC_RUN(a_bme688_is_named_as_one);
    HAC_RUN(init_reads_the_calibration_back_through_bosch);
    HAC_RUN(init_configures_the_sensor_as_the_datasheet_shows);
    HAC_RUN(init_resets_a_sensor_a_software_reset_left_measuring);
    HAC_RUN(init_waits_out_the_start_up_time);
    HAC_RUN(every_wait_is_long_enough_and_no_write_is_wasted);
    HAC_RUN(an_absent_sensor_fails_init_as_not_found);
    HAC_RUN(something_else_at_the_address_is_unsupported);
    HAC_RUN(every_call_of_init_refused_fails_it_and_names_its_step);
    HAC_RUN(a_reset_that_did_not_take_is_refused);
    HAC_RUN(a_reading_carries_the_offset_and_the_moved_humidity);
    HAC_RUN(the_burn_in_ends_exactly_when_it_says);
    HAC_RUN(before_it_is_done_the_bus_is_left_alone);
    HAC_RUN(suspend_drops_a_reading_not_collected);
    HAC_RUN(gas_not_marked_valid_is_not_a_reading);
    HAC_RUN(an_invalid_gas_value_is_an_error_even_during_burn_in);
    HAC_RUN(the_operating_range_bounds_temperature_and_takes_the_rest_with_it);
    HAC_RUN(the_operating_range_bounds_pressure_alone);
    HAC_RUN(humidity_at_its_limits_is_a_reading);
    HAC_RUN(humidity_moves_with_the_temperature_as_magnus_says);
    HAC_RUN(a_byte_refused_mid_read_is_an_io_error);
    HAC_RUN(a_sensor_that_stops_answering_is_not_found_and_logged_once);
    HAC_RUN(a_stalled_bus_is_a_timeout);
    HAC_RUN(a_little_slow_is_absorbed_by_bosch_polling);
    HAC_RUN(a_late_measurement_is_not_yet_within_the_grace);
    HAC_RUN(a_measurement_that_never_ends_is_a_timeout_not_a_hang);
    HAC_RUN(two_sensors_share_a_bus_without_sharing_state);
    HAC_RUN(setup_refuses_a_missing_bus_or_clock);
    HAC_RUN(the_hal_sleeps_at_least_as_long_as_asked);
    HAC_RUN(the_hal_refuses_what_it_cannot_send);
    HAC_RUN(the_hal_status_is_what_the_port_said);
    return hac_test_summary();
}
