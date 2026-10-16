// SPDX-License-Identifier: Apache-2.0
//
// bme68x_bsec_source -- Bosch's BSEC2 deciding when and how the BME68x
// measures, Bosch's Sensor API reading it -- over the fake bus and the BME680
// model, with BSEC's x86-64 Linux library linked (docs/adr/0020-bsec-iaq.md).
// Built only with HAC_BSEC: the library is not in this repository, so CI
// never runs this file. bme68x_bsec_status.c's mapping, which it relies on, is
// tested in every build (test_bme68x_bsec_status.c).
//
// What it proves: BSEC's heater profile and schedule reach the sensor's
// registers; BSEC is never called before its time and a late call is counted;
// BSEC's outputs come back as the sample with the statuses the ADR records,
// with no CO2 at all; failures look as they do in bme68x_source; what BSEC
// learnt survives a second init(); two sources do not share BSEC. Under the
// sensor manager, the way the firmware runs it, for simulated minutes.
//
// What it does not prove: anything about the air. The model hands back the
// gas resistance it is given, so BSEC's IAQ here is BSEC's answer to made-up
// numbers. Whether the index means anything on the board is the board's to
// show (docs/hardware/board_notes.md, open item "BSEC on the board").

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_i2c_fake.h"
#include "app/sensor_manager.h"
#include "bme68x_bsec_limits.h"
#include "bme68x_bsec_source.h"
#include "bme68x_bsec_status.h"
#include "bme68x_limits.h"
#include "hac_test.h"
#include "i2c_models/bme680_model.h"

#ifndef HAC_BSEC
#error "test_bme68x_bsec_source.c needs a build with HAC_BSEC (tests/CMakeLists.txt)"
#endif

#define ADDR 0x77u
#define ALT_ADDR 0x76u
#define SOURCE_ID 2u

// BSEC 2.6.1.0's power-on conditioning with this configuration, and the
// profile it then keeps (seen on the host; the ADR, "Timing").
#define CONDITIONING_HEATER_C 380u
#define CONDITIONING_HEATER_MS 1000u
#define LP_HEATER_C 320u
#define LP_HEATER_MS 197u

// --- a log that remembers ----------------------------------------------------

#define LOG_LINES 128u
#define LOG_LINE_LEN 240u

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
        switch (f->kind) {
            case LOG_FIELD_STR:
                used +=
                    (size_t)snprintf(line + used, LOG_LINE_LEN - used, " %s=%s", f->key, f->v.str);
                break;
            case LOG_FIELD_U64:
                used += (size_t)snprintf(line + used, LOG_LINE_LEN - used, " %s=%llu", f->key,
                                         (unsigned long long)f->v.u64);
                break;
            case LOG_FIELD_I64:
                used += (size_t)snprintf(line + used, LOG_LINE_LEN - used, " %s=%lld", f->key,
                                         (long long)f->v.i64);
                break;
            case LOG_FIELD_BOOL:
                used += (size_t)snprintf(line + used, LOG_LINE_LEN - used, " %s=%s", f->key,
                                         f->v.b ? "true" : "false");
                break;
            case LOG_FIELD_F32:
            default:
                used += (size_t)snprintf(line + used, LOG_LINE_LEN - used, " %s=%.1f", f->key,
                                         (double)f->v.f32);
                break;
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

// With HAC_TEST_VERBOSE set, a test's log is printed: what BSEC did, for
// reading rather than checking.
static void log_dump(const capture_log_t *c, const char *test)
{
    if (getenv("HAC_TEST_VERBOSE") == NULL) {
        return;
    }
    printf("--- %s\n", test);
    for (size_t i = 0; i < c->count; i++) {
        printf("%s\n", c->lines[i]);
    }
}

static bool log_has(const capture_log_t *c, const char *a, const char *b)
{
    for (size_t i = 0; i < c->count; i++) {
        if (strstr(c->lines[i], a) != NULL && (b == NULL || strstr(c->lines[i], b) != NULL)) {
            return true;
        }
    }
    return false;
}

// --- the air ---------------------------------------------------------------------

// A room: steady temperature, humidity and pressure, gas resistance with a
// little noise, and -- between `polluted_from_ms` and `polluted_until_ms` --
// at 40 %, as a burst of VOCs would leave it.
typedef struct {
    bme680_model_reading_t base;
    uint32_t noise;
    uint64_t polluted_from_ms;
    uint64_t polluted_until_ms;
} room_t;

static float noise(room_t *room)
{
    room->noise = room->noise * 1664525u + 1013904223u;
    return (float)(room->noise >> 8) / 16777216.0f - 0.5f;  // -0.5 .. 0.5
}

static void reading_from_room(void *ctx, uint64_t at_ms, bme680_model_reading_t *out)
{
    room_t *room = (room_t *)ctx;
    *out = room->base;
    out->gas_resistance_ohm *= 1.0f + 0.05f * noise(room);
    if (at_ms >= room->polluted_from_ms && at_ms < room->polluted_until_ms) {
        out->gas_resistance_ohm *= 0.4f;
    }
}

// --- the rig ---------------------------------------------------------------------

typedef struct {
    bme680_model_t sensor;
    room_t room;
    bme68x_bsec_source_t source;
    sensor_source_t *src;
} unit_t;

typedef struct {
    adp_clock_virtual_t clock;
    adp_i2c_fake_t bus;
    capture_log_t log;
    unit_t a;  // at ADDR
    unit_t b;  // at ALT_ADDR, for the test of two
} rig_t;

static rig_t g_rig;  // large -- two BSEC instances -- and every test makes one

static void unit_make(rig_t *r, unit_t *u, uint8_t addr, uint8_t id)
{
    bme680_model_init(&u->sensor, 0u);
    u->room = (room_t){
        .base = {.temperature_c = 24.3f,
                 .humidity_rh = 51.7f,
                 .pressure_hpa = 998.4f,
                 .gas_resistance_ohm = 87000.0f},
        .noise = 12345u + addr,
        .polluted_from_ms = UINT64_MAX,
        .polluted_until_ms = UINT64_MAX,
    };
    bme680_model_set_reading(&u->sensor, reading_from_room, &u->room);
    (void)adp_i2c_fake_attach(&r->bus, addr, bme680_model_port(&u->sensor));
    const bme68x_bsec_source_config_t cfg = {
        .bus = adp_i2c_fake_port(&r->bus),
        .clock = adp_clock_virtual_port(&r->clock),
        .log = &r->log.port,
        .address = addr,
        .id = id,
    };
    (void)bme68x_bsec_source_setup(&u->source, &cfg);
    u->src = &u->source.port;
}

static rig_t *rig_make(void)
{
    rig_t *r = &g_rig;
    memset(r, 0, sizeof *r);
    adp_clock_virtual_init(&r->clock, 1.0);
    adp_i2c_fake_init(&r->bus, &r->clock);
    r->log.port.write = capture_write;
    r->log.port.impl = &r->log;
    unit_make(r, &r->a, ADDR, SOURCE_ID);
    return r;
}

static void advance(rig_t *r, uint64_t ms)
{
    adp_clock_virtual_advance(&r->clock, ms);
}

static uint64_t now_of(rig_t *r)
{
    return r->clock.now_ms;
}

// Waits what ready_in_ms() asks and reads, until the source answers
// something other than "not yet".
static hal_status_t read_when_ready(rig_t *r, sensor_source_t *src, sensor_sample_t *out)
{
    hal_status_t status = HAL_ERR_NOT_READY;
    for (unsigned attempt = 0; attempt < 16u && status == HAL_ERR_NOT_READY; attempt++) {
        const uint32_t wait = src->ready_in_ms(src);
        advance(r, wait > 0u ? wait : 1u);
        status = src->read(src, out);
    }
    return status;
}

// The sensor manager over the rig's sources, as the firmware composes it.
typedef struct {
    sensor_manager_t mgr;
    air_reading_t reading;
    // What the IAQ field said, first to last.
    bool saw_iaq_warmup, saw_iaq_unreliable, saw_iaq_ok, iaq_ok_after_warmup;
    bool saw_gas_warmup, saw_gas_ok;
    bool saw_co2;
    uint32_t polls;
    uint32_t stalls;  // polls after which the loop was held
} run_t;

static run_t g_run;

static void manager_make(rig_t *r, run_t *run, unit_t *u)
{
    memset(run, 0, sizeof *run);
    const sensor_manager_config_t cfg = sensor_manager_config_default();
    sensor_manager_init(&run->mgr, adp_clock_virtual_port(&r->clock), NULL, &cfg);
    HAC_CHECK_EQ_INT(sensor_manager_add(&run->mgr, u->src, HAC_BSEC_PERIOD_MS), HAL_OK);
    air_reading_init(&run->reading);
}

// Polls the manager at its deadlines for `ms`, holding the loop `stall_ms`
// longer every `stall_every` polls -- as the firmware's event loop is held by
// a refresh of the panel.
static void manager_run(rig_t *r, run_t *run, uint64_t ms, uint32_t stall_every, uint64_t stall_ms)
{
    const uint64_t end = now_of(r) + ms;
    while (now_of(r) < end) {
        const uint64_t due = sensor_manager_poll(&run->mgr, &run->reading);
        run->polls++;
        const measurement_f32_t *iaq = &run->reading.iaq;
        if (iaq->status == VAL_WARMUP) {
            run->saw_iaq_warmup = true;
            run->saw_iaq_unreliable = run->saw_iaq_unreliable || iaq->accuracy == ACC_UNRELIABLE;
        }
        if (iaq->status == VAL_OK) {
            run->iaq_ok_after_warmup = run->iaq_ok_after_warmup || run->saw_iaq_warmup;
            run->saw_iaq_ok = true;
        }
        run->saw_gas_warmup =
            run->saw_gas_warmup || run->reading.gas_resistance_ohm.status == VAL_WARMUP;
        run->saw_gas_ok = run->saw_gas_ok || run->reading.gas_resistance_ohm.status == VAL_OK;
        run->saw_co2 = run->saw_co2 || run->reading.co2_ppm.status != VAL_UNAVAILABLE;

        uint64_t next = (due > now_of(r)) ? due : now_of(r) + 1u;
        if (stall_every != 0u && run->polls % stall_every == 0u) {
            next += stall_ms;
            run->stalls++;
        }
        advance(r, next - now_of(r));
    }
}

static bool all_five_error(const sensor_sample_t *s)
{
    return s->temperature_c.status == VAL_ERROR && s->humidity_rh.status == VAL_ERROR &&
           s->pressure_hpa.status == VAL_ERROR && s->gas_resistance_ohm.status == VAL_ERROR &&
           s->iaq.status == VAL_ERROR;
}

// What gas_wait_0 holds, as a duration (BME680 datasheet 5.3.3.3).
static uint32_t gas_wait_ms(const bme680_model_t *m)
{
    static const uint32_t FACTOR[] = {1u, 4u, 16u, 64u};
    const uint8_t v = bme680_model_peek(m, BME680_MODEL_REG_GAS_WAIT_0);
    return (uint32_t)(v & 0x3Fu) * FACTOR[v >> 6];
}

// --- capabilities and init ---------------------------------------------------------

static void it_claims_iaq_and_never_co2(void)
{
    rig_t *r = rig_make();
    const sensor_caps_t caps = r->a.src->caps;
    HAC_CHECK((caps & SENS_CAP_IAQ) != 0u);
    HAC_CHECK((caps & SENS_CAP_TEMPERATURE) != 0u);
    HAC_CHECK((caps & SENS_CAP_HUMIDITY) != 0u);
    HAC_CHECK((caps & SENS_CAP_PRESSURE) != 0u);
    HAC_CHECK((caps & SENS_CAP_GAS_RES) != 0u);
    HAC_CHECK((caps & SENS_CAP_CO2) == 0u);  // requirements 4.1: eCO2 is not CO2
    HAC_CHECK((caps & SENS_CAP_VOC) == 0u);
    HAC_CHECK(strcmp(r->a.src->name, "bme68x_bsec") == 0);
}

static void init_names_bsec_its_configuration_and_a_fresh_state(void)
{
    rig_t *r = rig_make();
    HAC_CHECK_EQ_INT(r->a.src->init(r->a.src), HAL_OK);
    HAC_CHECK(log_has(&r->log, "sensor ready", "bsec=2.6.1.0"));
    HAC_CHECK(log_has(&r->log, "sensor ready", "config=" HAC_BSEC_TEST_CONFIG_NAME));
    HAC_CHECK(log_has(&r->log, "sensor ready", "variant=BME680"));
    HAC_CHECK(log_has(&r->log, "sensor ready", "state=fresh"));
    HAC_CHECK_EQ_INT(log_count(&r->log, "calibration ("), 5);
}

static void an_absent_sensor_fails_init_and_bsec_is_not_started(void)
{
    rig_t *r = rig_make();
    r->a.sensor.faults.nack_address = 1000u;
    HAC_CHECK_EQ_INT(r->a.src->init(r->a.src), HAL_ERR_NOT_FOUND);
    HAC_CHECK(!r->a.source.bsec_started);
    HAC_CHECK(log_has(&r->log, "sensor not usable", "step=init"));
}

static void setup_refuses_a_missing_bus_or_clock(void)
{
    bme68x_bsec_source_t s;
    bme68x_bsec_source_config_t cfg = {0};
    HAC_CHECK_EQ_INT(bme68x_bsec_source_setup(&s, &cfg), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(bme68x_bsec_source_setup(NULL, &cfg), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(bme68x_bsec_source_setup(&s, NULL), HAL_ERR_INVALID_ARG);
}

// --- BSEC's schedule ------------------------------------------------------------

static void before_start_nothing_touches_the_sensor(void)
{
    rig_t *r = rig_make();
    HAC_CHECK_EQ_INT(r->a.src->init(r->a.src), HAL_OK);
    const uint32_t calls = r->a.sensor.stats.calls;
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(r->a.src->read(r->a.src, &s), HAL_ERR_NOT_READY);
    HAC_CHECK_EQ_INT(r->a.src->ready_in_ms(r->a.src), 0);
    HAC_CHECK_EQ_INT((int)r->a.sensor.stats.calls, (int)calls);
    HAC_CHECK_EQ_INT((int)r->a.source.measurements, 0);
}

static void the_first_measurement_is_bsec_s_conditioning_profile(void)
{
    rig_t *r = rig_make();
    HAC_CHECK_EQ_INT(r->a.src->init(r->a.src), HAL_OK);
    HAC_CHECK_EQ_INT(r->a.src->start(r->a.src), HAL_OK);

    HAC_CHECK_EQ_INT((int)r->a.source.measurements, 1);
    HAC_CHECK(r->a.sensor.measuring);
    // The heater's target and time, as BSEC asked, in the sensor's registers:
    // the target as Bosch's code encodes it for 25 C ambient, the time as
    // the sensor's 6-bit-and-multiplier register can hold 1000 ms.
    HAC_CHECK_EQ_INT(bme680_model_peek(&r->a.sensor, BME680_MODEL_REG_RES_HEAT_0),
                     bme680_model_res_heat(&r->a.sensor.calibration, CONDITIONING_HEATER_C,
                                           HAC_BME68X_HEATER_AMBIENT_C));
    HAC_CHECK(gas_wait_ms(&r->a.sensor) <= CONDITIONING_HEATER_MS);
    HAC_CHECK(gas_wait_ms(&r->a.sensor) >= CONDITIONING_HEATER_MS - 16u);
    HAC_CHECK(log_has(&r->log, "heater profile", "heater_c=380 heater_ms=1000"));

    // Read when the heater's second and the conversions are done, and not
    // before.
    const uint32_t wait = r->a.src->ready_in_ms(r->a.src);
    HAC_CHECK(wait > CONDITIONING_HEATER_MS);
    HAC_CHECK(wait < CONDITIONING_HEATER_MS + 100u);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(read_when_ready(r, r->a.src, &s), HAL_OK);
    HAC_CHECK_EQ_INT((int)r->a.sensor.stats.early_reads, 0);
    HAC_CHECK_EQ_INT((int)r->a.sensor.stats.ignored_writes, 0);
}

static void after_a_reading_it_waits_for_bsec_s_next_call(void)
{
    rig_t *r = rig_make();
    HAC_CHECK_EQ_INT(r->a.src->init(r->a.src), HAL_OK);
    const uint64_t first_call = now_of(r);
    HAC_CHECK_EQ_INT(r->a.src->start(r->a.src), HAL_OK);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(read_when_ready(r, r->a.src, &s), HAL_OK);

    // Counted from the call, not from the reading.
    HAC_CHECK_EQ_U64(r->a.source.next_call_ms, first_call + HAC_BSEC_PERIOD_MS);
    HAC_CHECK_EQ_U64(r->a.src->ready_in_ms(r->a.src), first_call + HAC_BSEC_PERIOD_MS - now_of(r));
}

static void bsec_is_never_called_before_its_time(void)
{
    rig_t *r = rig_make();
    HAC_CHECK_EQ_INT(r->a.src->init(r->a.src), HAL_OK);
    HAC_CHECK_EQ_INT(r->a.src->start(r->a.src), HAL_OK);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(read_when_ready(r, r->a.src, &s), HAL_OK);

    // A start() and reads a millisecond before BSEC's time: nothing happens.
    advance(r, r->a.src->ready_in_ms(r->a.src) - 1u);
    HAC_CHECK_EQ_INT(r->a.src->start(r->a.src), HAL_OK);
    HAC_CHECK_EQ_INT(r->a.src->read(r->a.src, &s), HAL_ERR_NOT_READY);
    HAC_CHECK_EQ_INT((int)r->a.source.measurements, 1);
    HAC_CHECK_EQ_INT(r->a.src->ready_in_ms(r->a.src), 1);

    // At it, read() makes the call and starts the measurement.
    advance(r, 1u);
    HAC_CHECK_EQ_INT(r->a.src->read(r->a.src, &s), HAL_ERR_NOT_READY);
    HAC_CHECK_EQ_INT((int)r->a.source.measurements, 2);
    HAC_CHECK_EQ_INT((int)r->a.source.late_calls, 0);
    HAC_CHECK_EQ_INT(log_count(&r->log, "BSEC called late"), 0);
}

static void a_late_call_is_counted_and_logged_with_how_late(void)
{
    rig_t *r = rig_make();
    HAC_CHECK_EQ_INT(r->a.src->init(r->a.src), HAL_OK);
    HAC_CHECK_EQ_INT(r->a.src->start(r->a.src), HAL_OK);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(read_when_ready(r, r->a.src, &s), HAL_OK);

    // Within BSEC's 6.25 %: not late.
    advance(r, r->a.src->ready_in_ms(r->a.src) + 150u);
    HAC_CHECK_EQ_INT(r->a.src->start(r->a.src), HAL_OK);
    HAC_CHECK_EQ_INT(read_when_ready(r, r->a.src, &s), HAL_OK);
    HAC_CHECK_EQ_INT((int)r->a.source.late_calls, 0);

    // A full refresh's worth late: answered, measured, counted.
    advance(r, r->a.src->ready_in_ms(r->a.src) + 3700u);
    HAC_CHECK_EQ_INT(r->a.src->start(r->a.src), HAL_OK);
    HAC_CHECK_EQ_INT((int)r->a.source.measurements, 3);
    HAC_CHECK_EQ_INT((int)r->a.source.late_calls, 1);
    HAC_CHECK(log_has(&r->log, "BSEC called late", "late_ms=3700"));
    HAC_CHECK_EQ_INT(read_when_ready(r, r->a.src, &s), HAL_OK);
    HAC_CHECK(s.temperature_c.status == VAL_OK);
}

// --- what comes back -----------------------------------------------------------

static void under_the_manager_iaq_warms_up_and_then_reads(void)
{
    rig_t *r = rig_make();
    run_t *run = &g_run;
    manager_make(r, run, &r->a);
    // Clean air, then a burst of VOCs, then clean again.
    r->a.room.polluted_from_ms = 20u * 60000u;
    r->a.room.polluted_until_ms = 25u * 60000u;
    manager_run(r, run, 40u * 60000u, 0u, 0u);

    const bme68x_bsec_source_t *s = &r->a.source;
    // One measurement per BSEC period, every call on time.
    HAC_CHECK(s->measurements >= (40u * 60000u) / HAC_BSEC_PERIOD_MS - 2u);
    HAC_CHECK(s->measurements <= (40u * 60000u) / HAC_BSEC_PERIOD_MS + 1u);
    HAC_CHECK_EQ_INT((int)s->late_calls, 0);
    HAC_CHECK_EQ_INT((int)r->a.sensor.stats.early_reads, 0);
    HAC_CHECK_EQ_INT((int)r->a.sensor.stats.ignored_writes, 0);

    // IAQ: a warm-up first, then a reading, carrying BSEC's accuracy on the
    // core's scale.
    HAC_CHECK(run->saw_iaq_warmup);
    HAC_CHECK(run->saw_iaq_unreliable);
    HAC_CHECK_EQ_INT(run->reading.iaq.accuracy, bme68x_bsec_iaq_accuracy(s->iaq_accuracy));
    HAC_CHECK(run->iaq_ok_after_warmup);
    HAC_CHECK(log_has(&r->log, "iaq accuracy", "to=low"));
    HAC_CHECK(s->iaq_accuracy >= 1u);
    // The burst moved it.
    HAC_CHECK(log_has(&r->log, "iaq accuracy", "to=medium"));

    // Gas: warming up until BSEC's run-in is done, then a reading.
    HAC_CHECK(run->saw_gas_warmup);
    HAC_CHECK(run->saw_gas_ok);
    HAC_CHECK(s->stabilized && s->run_in_done);

    // BSEC settled on its low-power profile.
    HAC_CHECK(log_has(&r->log, "heater profile", "heater_c=320 heater_ms=197"));
    HAC_CHECK_EQ_INT(s->heater_temp_c, LP_HEATER_C);
    HAC_CHECK_EQ_INT(s->heater_dur_ms, LP_HEATER_MS);

    // Never CO2, whatever BSEC estimates.
    HAC_CHECK(!run->saw_co2);
    HAC_CHECK(run->reading.voc_index.status == VAL_UNAVAILABLE);

    // Temperature is BSEC's heat-compensated value: below the die's, by
    // BSEC's own estimate of the heater's share. Pressure is the sensor's.
    HAC_CHECK(run->reading.temperature_c.status == VAL_OK);
    HAC_CHECK(run->reading.temperature_c.value < r->a.room.base.temperature_c);
    HAC_CHECK(run->reading.temperature_c.value > r->a.room.base.temperature_c - 3.0f);
    HAC_CHECK(run->reading.humidity_rh.status == VAL_OK);
    HAC_CHECK(run->reading.humidity_rh.value >
              r->a.room.base.humidity_rh);  // warmer air holds more
    HAC_CHECK_EQ_F32(run->reading.pressure_hpa.value, r->a.room.base.pressure_hpa, 0.05f);
    HAC_CHECK_EQ_INT(run->reading.iaq.source_id, SOURCE_ID);
    log_dump(&r->log, __func__);
}

static void a_loop_held_by_full_refreshes_is_late_and_bsec_still_reads(void)
{
    rig_t *r = rig_make();
    run_t *run = &g_run;
    manager_make(r, run, &r->a);
    // About one hold of 3.7 s every 100 s -- a full refresh, at most every
    // 105 s in the firmware (ADR 0019) -- landing where it may.
    manager_run(r, run, 20u * 60000u, 67u, 3700u);

    // A hold longer than BSEC's period always covers one of its calls, so
    // each makes at most one late -- and the next is on time again, since
    // BSEC counts from the call.
    const bme68x_bsec_source_t *s = &r->a.source;
    HAC_CHECK(s->late_calls > 0u);
    HAC_CHECK(s->late_calls <= run->stalls);
    HAC_CHECK(run->iaq_ok_after_warmup);  // BSEC carried on
    HAC_CHECK(log_has(&r->log, "BSEC called late", "late_calls=1"));
    log_dump(&r->log, __func__);
}

static void gas_the_sensor_did_not_mark_valid_takes_iaq_with_it(void)
{
    rig_t *r = rig_make();
    HAC_CHECK_EQ_INT(r->a.src->init(r->a.src), HAL_OK);
    r->a.sensor.faults.gas_invalid = true;
    HAC_CHECK_EQ_INT(r->a.src->start(r->a.src), HAL_OK);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(read_when_ready(r, r->a.src, &s), HAL_OK);
    HAC_CHECK(s.gas_resistance_ohm.status == VAL_ERROR);
    HAC_CHECK(s.iaq.status == VAL_ERROR);
    HAC_CHECK(s.temperature_c.status == VAL_OK);
    HAC_CHECK(s.humidity_rh.status == VAL_OK);
    HAC_CHECK(s.pressure_hpa.status == VAL_OK);
    HAC_CHECK(s.co2_ppm.status == VAL_UNAVAILABLE);
    HAC_CHECK(log_has(&r->log, "gas not measured", "gas_valid=false"));
}

static void a_temperature_outside_the_range_is_not_handed_to_bsec(void)
{
    rig_t *r = rig_make();
    HAC_CHECK_EQ_INT(r->a.src->init(r->a.src), HAL_OK);
    r->a.room.base.temperature_c = 86.0f;
    HAC_CHECK_EQ_INT(r->a.src->start(r->a.src), HAL_OK);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(read_when_ready(r, r->a.src, &s), HAL_OK);
    HAC_CHECK(all_five_error(&s));
    HAC_CHECK(s.co2_ppm.status == VAL_UNAVAILABLE);
    // BSEC saw nothing to judge.
    HAC_CHECK(!r->a.source.have_accuracy);
}

// --- failures ------------------------------------------------------------------

static void a_sensor_that_stops_answering_is_not_found_and_logged_once(void)
{
    rig_t *r = rig_make();
    HAC_CHECK_EQ_INT(r->a.src->init(r->a.src), HAL_OK);
    HAC_CHECK_EQ_INT(r->a.src->start(r->a.src), HAL_OK);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(read_when_ready(r, r->a.src, &s), HAL_OK);

    r->a.sensor.faults.nack_address = 1000u;
    for (int i = 0; i < 3; i++) {
        advance(r, r->a.src->ready_in_ms(r->a.src));
        HAC_CHECK_EQ_INT(r->a.src->read(r->a.src, &s), HAL_ERR_NOT_FOUND);
        HAC_CHECK(all_five_error(&s));
    }
    HAC_CHECK_EQ_INT(log_count(&r->log, "read failed"), 1);
}

static void a_measurement_that_never_ends_is_a_timeout_not_a_hang(void)
{
    rig_t *r = rig_make();
    HAC_CHECK_EQ_INT(r->a.src->init(r->a.src), HAL_OK);
    r->a.sensor.faults.overrun_ms = BME680_MODEL_NEVER;
    HAC_CHECK_EQ_INT(r->a.src->start(r->a.src), HAL_OK);
    sensor_sample_t s;
    hal_status_t status = HAL_ERR_NOT_READY;
    for (int i = 0; i < 40 && status == HAL_ERR_NOT_READY; i++) {
        advance(r, r->a.src->ready_in_ms(r->a.src) + 1u);
        status = r->a.src->read(r->a.src, &s);
    }
    HAC_CHECK_EQ_INT(status, HAL_ERR_TIMEOUT);
    HAC_CHECK(all_five_error(&s));
    HAC_CHECK(log_has(&r->log, "read failed", "step=measurement_overdue"));
}

// --- BSEC's state ----------------------------------------------------------------

static void init_again_keeps_what_bsec_learnt_since_boot(void)
{
    rig_t *r = rig_make();
    run_t *run = &g_run;
    manager_make(r, run, &r->a);
    manager_run(r, run, 10u * 60000u, 0u, 0u);
    HAC_CHECK(r->a.source.iaq_accuracy >= 1u);
    const uint8_t accuracy = r->a.source.iaq_accuracy;

    // The sensor unplugged and back: a power cycle, and the manager's init().
    bme680_model_power_cycle(&r->a.sensor, now_of(r));
    advance(r, 30000u);
    r->log.count = 0u;
    HAC_CHECK_EQ_INT(r->a.src->init(r->a.src), HAL_OK);
    HAC_CHECK(log_has(&r->log, "sensor ready", "state=kept_since_boot"));
    HAC_CHECK_EQ_INT(log_count(&r->log, "calibration ("), 0);  // logged once per boot

    HAC_CHECK_EQ_INT(r->a.src->start(r->a.src), HAL_OK);
    sensor_sample_t s;
    HAC_CHECK_EQ_INT(read_when_ready(r, r->a.src, &s), HAL_OK);
    HAC_CHECK(r->a.source.have_accuracy);
    HAC_CHECK_EQ_INT(r->a.source.iaq_accuracy, accuracy);
    HAC_CHECK(s.iaq.status == VAL_OK);
    log_dump(&r->log, __func__);
}

static void two_sources_do_not_share_bsec(void)
{
    rig_t *r = rig_make();
    unit_make(r, &r->b, ALT_ADDR, SOURCE_ID + 1u);
    run_t *run = &g_run;
    manager_make(r, run, &r->a);
    manager_run(r, run, 10u * 60000u, 0u, 0u);
    HAC_CHECK(r->a.source.iaq_accuracy >= 1u);

    // A second sensor, its own instance: BSEC starts from nothing there,
    // while the first carries on where it was.
    HAC_CHECK_EQ_INT(sensor_manager_add(&run->mgr, r->b.src, HAC_BSEC_PERIOD_MS), HAL_OK);
    manager_run(r, run, 30000u, 0u, 0u);
    HAC_CHECK(r->b.source.measurements >= 9u);
    HAC_CHECK(r->b.source.have_accuracy);
    HAC_CHECK_EQ_INT(r->b.source.iaq_accuracy, 0);
    HAC_CHECK(log_has(&r->log, "sensor ready", "state=fresh"));
    HAC_CHECK(r->a.source.iaq_accuracy >= 1u);
    HAC_CHECK_EQ_INT((int)r->a.source.late_calls, 0);
    HAC_CHECK_EQ_INT((int)r->b.source.late_calls, 0);
}

int main(void)
{
    HAC_RUN(it_claims_iaq_and_never_co2);
    HAC_RUN(init_names_bsec_its_configuration_and_a_fresh_state);
    HAC_RUN(an_absent_sensor_fails_init_and_bsec_is_not_started);
    HAC_RUN(setup_refuses_a_missing_bus_or_clock);
    HAC_RUN(before_start_nothing_touches_the_sensor);
    HAC_RUN(the_first_measurement_is_bsec_s_conditioning_profile);
    HAC_RUN(after_a_reading_it_waits_for_bsec_s_next_call);
    HAC_RUN(bsec_is_never_called_before_its_time);
    HAC_RUN(a_late_call_is_counted_and_logged_with_how_late);
    HAC_RUN(under_the_manager_iaq_warms_up_and_then_reads);
    HAC_RUN(a_loop_held_by_full_refreshes_is_late_and_bsec_still_reads);
    HAC_RUN(gas_the_sensor_did_not_mark_valid_takes_iaq_with_it);
    HAC_RUN(a_temperature_outside_the_range_is_not_handed_to_bsec);
    HAC_RUN(a_sensor_that_stops_answering_is_not_found_and_logged_once);
    HAC_RUN(a_measurement_that_never_ends_is_a_timeout_not_a_hang);
    HAC_RUN(init_again_keeps_what_bsec_learnt_since_boot);
    HAC_RUN(two_sources_do_not_share_bsec);
    return hac_test_summary();
}
