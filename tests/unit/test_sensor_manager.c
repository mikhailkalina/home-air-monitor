// SPDX-License-Identifier: Apache-2.0
//
// core/app/sensor_manager.c over replay sources -- one standing in for the
// SCD41 (CO2, temperature, humidity), one for the BME68x (temperature,
// humidity, pressure, gas) -- each wrapped in fault_inject so a test can
// break it mid-run, on the fake clock, so an hour of backoff costs
// microseconds. The ids and priorities are this file's, as the composition's
// are on the board: nothing in the manager knows which sensor is which.

#ifndef HAC_TEST_TMP_DIR
#error "HAC_TEST_TMP_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif

#include "app/sensor_manager.h"

#include <stdio.h>
#include <string.h>

#include "fake_clock.h"
#include "hac_test.h"
#include "replay/fault_inject.h"
#include "replay/replay_source.h"

#define CO2_ID 1u
#define ENV_ID 2u

#define INTERVAL_MS 5000u

// The two stand-ins report different temperatures and humidities, so a test
// can tell from the value alone which one the merge took.
#define CO2_SOURCE_PPM 600.0f
#define CO2_SOURCE_T_C 25.0f
#define CO2_SOURCE_RH 40.0f
#define ENV_SOURCE_T_C 21.0f
#define ENV_SOURCE_RH 50.0f
#define ENV_SOURCE_HPA 1000.0f
#define ENV_SOURCE_GAS_OHM 100000.0f

#define VALUE_EPS 0.001f

// A poll may legitimately return "now" when a source used up its steps; it
// must never do so for long, or the caller would spin.
#define MAX_POLLS_AT_ONE_INSTANT 2u

static char *write_temp_csv(const char *name, const char *content)
{
    static char path[512];
    snprintf(path, sizeof(path), "%s/%s", HAC_TEST_TMP_DIR, name);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return NULL;
    }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return path;
}

typedef struct {
    fake_clock_t clock;
    replay_source_t co2_replay;
    replay_source_t env_replay;
    fault_inject_t co2;
    fault_inject_t env;
    sensor_manager_t mgr;
    air_reading_t reading;
    uint64_t deadline;
    uint32_t polls;
    bool spun;  // a poll returned "now" more than MAX_POLLS_AT_ONE_INSTANT times in a row
} fixture_t;

static const uint8_t PREFER_ENV[] = {ENV_ID, CO2_ID};
static const uint8_t ONLY_ENV[] = {ENV_ID};
static const uint8_t ONLY_CO2[] = {CO2_ID};

static void set_board_priorities(sensor_manager_t *m)
{
    // docs/architecture.md 6.3: temperature and humidity from the BME68x,
    // the SCD41 as fallback; CO2 from the SCD41 alone; the rest from the
    // BME68x alone.
    HAC_CHECK_EQ_INT(sensor_manager_set_priority(m, SENSOR_FIELD_TEMPERATURE, PREFER_ENV, 2u),
                     HAL_OK);
    HAC_CHECK_EQ_INT(sensor_manager_set_priority(m, SENSOR_FIELD_HUMIDITY, PREFER_ENV, 2u), HAL_OK);
    HAC_CHECK_EQ_INT(sensor_manager_set_priority(m, SENSOR_FIELD_PRESSURE, ONLY_ENV, 1u), HAL_OK);
    HAC_CHECK_EQ_INT(sensor_manager_set_priority(m, SENSOR_FIELD_CO2, ONLY_CO2, 1u), HAL_OK);
    HAC_CHECK_EQ_INT(sensor_manager_set_priority(m, SENSOR_FIELD_GAS_RESISTANCE, ONLY_ENV, 1u),
                     HAL_OK);
}

static void fixture_init(fixture_t *f, const sensor_manager_config_t *cfg)
{
    memset(f, 0, sizeof(*f));
    fake_clock_init(&f->clock);
    const port_clock_t *clock = fake_clock_port(&f->clock);

    char *co2_path =
        write_temp_csv("mgr_co2.csv", "t_offset_s,co2_ppm,temperature_c,humidity_rh,status\n"
                                      "0,600,25.0,40.0,ok\n"
                                      "3600,600,25.0,40.0,ok\n");
    HAC_CHECK(co2_path != NULL);
    HAC_CHECK_EQ_INT(replay_source_init(&f->co2_replay, co2_path,
                                        SENS_CAP_CO2 | SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY,
                                        CO2_ID, clock, false),
                     HAL_OK);

    char *env_path = write_temp_csv(
        "mgr_env.csv", "t_offset_s,temperature_c,humidity_rh,pressure_hpa,gas_res_ohm,status\n"
                       "0,21.0,50.0,1000.0,100000,ok\n"
                       "3600,21.0,50.0,1000.0,100000,ok\n");
    HAC_CHECK(env_path != NULL);
    HAC_CHECK_EQ_INT(replay_source_init(&f->env_replay, env_path,
                                        SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY |
                                            SENS_CAP_PRESSURE | SENS_CAP_GAS_RES,
                                        ENV_ID, clock, false),
                     HAL_OK);

    fault_inject_init(&f->co2, &f->co2_replay.port);
    fault_inject_init(&f->env, &f->env_replay.port);

    sensor_manager_init(&f->mgr, clock, NULL, cfg);
    HAC_CHECK_EQ_INT(sensor_manager_add(&f->mgr, &f->co2.port, INTERVAL_MS), HAL_OK);
    HAC_CHECK_EQ_INT(sensor_manager_add(&f->mgr, &f->env.port, INTERVAL_MS), HAL_OK);
    set_board_priorities(&f->mgr);
    air_reading_init(&f->reading);
}

static void fixture_default(fixture_t *f)
{
    const sensor_manager_config_t cfg = sensor_manager_config_default();
    fixture_init(f, &cfg);
}

static void fixture_dispose(fixture_t *f)
{
    replay_source_deinit(&f->co2_replay);
    replay_source_deinit(&f->env_replay);
}

static void poll(fixture_t *f)
{
    f->deadline = sensor_manager_poll(&f->mgr, &f->reading);
    f->polls++;
    HAC_CHECK(f->deadline >= f->clock.now_ms);  // never a deadline already passed
}

// Polls at every deadline the manager names until the clock reaches
// `end_ms`, then once more at `end_ms`, as an event loop would.
static void run_until(fixture_t *f, uint64_t end_ms)
{
    unsigned at_one_instant = 0u;
    poll(f);
    while (f->deadline <= end_ms) {
        if (f->deadline > f->clock.now_ms) {
            fake_clock_advance(&f->clock, f->deadline - f->clock.now_ms);
            at_one_instant = 0u;
        } else if (++at_one_instant > MAX_POLLS_AT_ONE_INSTANT) {
            f->spun = true;
            fake_clock_advance(&f->clock, 1u);  // keep the test finite
        }
        poll(f);
    }
    if (f->clock.now_ms < end_ms) {
        fake_clock_advance(&f->clock, end_ms - f->clock.now_ms);
        poll(f);
    }
}

static void run_for(fixture_t *f, uint64_t ms)
{
    run_until(f, f->clock.now_ms + ms);
}

static const sensor_manager_slot_t *slot(const fixture_t *f, size_t index)
{
    return sensor_manager_slot(&f->mgr, index);
}

// --- registration ------------------------------------------------------------

static void add_refuses_what_it_could_not_tell_apart(void)
{
    fixture_t f;
    fixture_default(&f);

    // Same id twice: the merge could not tell the two apart.
    HAC_CHECK_EQ_INT(sensor_manager_add(&f.mgr, &f.co2.port, INTERVAL_MS), HAL_ERR_INVALID_ARG);

    sensor_source_t nameless = f.co2_replay.port;
    nameless.id = 0u;  // "no source" in a measurement
    HAC_CHECK_EQ_INT(sensor_manager_add(&f.mgr, &nameless, INTERVAL_MS), HAL_ERR_INVALID_ARG);

    sensor_source_t other = f.co2_replay.port;
    other.id = 3u;
    HAC_CHECK_EQ_INT(sensor_manager_add(&f.mgr, &other, 0u), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(sensor_manager_add(&f.mgr, NULL, INTERVAL_MS), HAL_ERR_INVALID_ARG);

    HAC_CHECK_EQ_INT(sensor_manager_add(&f.mgr, &other, INTERVAL_MS), HAL_OK);
    sensor_source_t fourth = f.co2_replay.port;
    fourth.id = 4u;
    HAC_CHECK_EQ_INT(sensor_manager_add(&f.mgr, &fourth, INTERVAL_MS), HAL_OK);
    sensor_source_t fifth = f.co2_replay.port;
    fifth.id = 5u;
    HAC_CHECK_EQ_INT(sensor_manager_add(&f.mgr, &fifth, INTERVAL_MS), HAL_ERR_NO_MEM);

    const uint8_t zero[] = {0u};
    HAC_CHECK_EQ_INT(sensor_manager_set_priority(&f.mgr, SENSOR_FIELD_CO2, zero, 1u),
                     HAL_ERR_INVALID_ARG);
    const uint8_t too_many[] = {1u, 2u, 3u, 4u, 5u};
    HAC_CHECK_EQ_INT(sensor_manager_set_priority(&f.mgr, SENSOR_FIELD_CO2, too_many, 5u),
                     HAL_ERR_INVALID_ARG);
    fixture_dispose(&f);
}

static void an_empty_manager_has_no_deadline_and_touches_nothing(void)
{
    fake_clock_t clock;
    fake_clock_init(&clock);
    sensor_manager_t mgr;
    const sensor_manager_config_t cfg = sensor_manager_config_default();
    sensor_manager_init(&mgr, fake_clock_port(&clock), NULL, &cfg);

    air_reading_t reading;
    air_reading_init(&reading);
    reading.battery_percent = measurement_ok(80.0f, 0u, 9u);

    HAC_CHECK_EQ_U64(sensor_manager_poll(&mgr, &reading), SENSOR_MANAGER_NO_DEADLINE);
    HAC_CHECK_EQ_INT(reading.co2_ppm.status, VAL_UNAVAILABLE);
    HAC_CHECK_EQ_INT(reading.battery_percent.status, VAL_OK);  // not a sensor field
}

// --- the cycle ---------------------------------------------------------------

static void the_first_poll_initialises_starts_and_reads_every_source(void)
{
    fixture_t f;
    fixture_default(&f);
    poll(&f);

    HAC_CHECK_EQ_INT((int)f.co2.calls.inits, 1);
    HAC_CHECK_EQ_INT((int)f.env.calls.inits, 1);
    HAC_CHECK_EQ_INT((int)slot(&f, 0)->reads, 1);  // a replay source is ready at once
    HAC_CHECK_EQ_INT((int)slot(&f, 1)->reads, 1);

    HAC_CHECK_EQ_INT(f.reading.co2_ppm.status, VAL_OK);
    HAC_CHECK_EQ_F32(f.reading.co2_ppm.value, CO2_SOURCE_PPM, VALUE_EPS);
    HAC_CHECK_EQ_INT(f.reading.co2_ppm.source_id, CO2_ID);
    HAC_CHECK_EQ_F32(f.reading.temperature_c.value, ENV_SOURCE_T_C, VALUE_EPS);
    HAC_CHECK_EQ_INT(f.reading.temperature_c.source_id, ENV_ID);
    HAC_CHECK_EQ_F32(f.reading.humidity_rh.value, ENV_SOURCE_RH, VALUE_EPS);
    HAC_CHECK_EQ_F32(f.reading.pressure_hpa.value, ENV_SOURCE_HPA, VALUE_EPS);
    HAC_CHECK_EQ_F32(f.reading.gas_resistance_ohm.value, ENV_SOURCE_GAS_OHM, 1.0f);
    HAC_CHECK_EQ_INT(f.reading.iaq.status, VAL_UNAVAILABLE);  // nobody claims it

    HAC_CHECK_EQ_U64(f.deadline, INTERVAL_MS);  // the next cycle
    fixture_dispose(&f);
}

static void each_source_is_read_once_per_interval(void)
{
    fixture_t f;
    fixture_default(&f);
    run_until(&f, 60000u);

    // t = 0, 5, ..., 60 s.
    HAC_CHECK_EQ_INT((int)slot(&f, 0)->reads, 13);
    HAC_CHECK_EQ_INT((int)slot(&f, 1)->reads, 13);
    HAC_CHECK_EQ_INT((int)f.env.calls.inits, 1);
    HAC_CHECK(!f.spun);
    HAC_CHECK_EQ_U64(f.reading.co2_ppm.ts_ms, 60000u);
    fixture_dispose(&f);
}

static void a_field_without_priority_takes_the_sources_in_the_order_added(void)
{
    fixture_t f;
    fixture_default(&f);
    // No priority for temperature at all: the first source added -- the CO2
    // stand-in here -- wins a tie of two VAL_OK values.
    HAC_CHECK_EQ_INT(sensor_manager_set_priority(&f.mgr, SENSOR_FIELD_TEMPERATURE, NULL, 0u),
                     HAL_OK);
    poll(&f);
    HAC_CHECK_EQ_INT(f.reading.temperature_c.source_id, CO2_ID);
    HAC_CHECK_EQ_F32(f.reading.temperature_c.value, CO2_SOURCE_T_C, VALUE_EPS);
    fixture_dispose(&f);
}

static void a_priority_may_name_a_source_that_does_not_exist(void)
{
    fixture_t f;
    fixture_default(&f);
    const uint8_t ghost_first[] = {7u, CO2_ID};
    HAC_CHECK_EQ_INT(sensor_manager_set_priority(&f.mgr, SENSOR_FIELD_HUMIDITY, ghost_first, 2u),
                     HAL_OK);
    poll(&f);
    HAC_CHECK_EQ_INT(f.reading.humidity_rh.source_id, CO2_ID);
    fixture_dispose(&f);
}

static void a_source_cannot_fill_a_field_it_does_not_claim(void)
{
    fixture_t f;
    fixture_default(&f);
    // The CO2 stand-in's file has temperature, but this source says it
    // measures CO2 only: its temperature must never reach the reading.
    f.co2_replay.port.caps = SENS_CAP_CO2;
    f.co2.port.caps = SENS_CAP_CO2;
    fault_inject_set(&f.env, FAULT_BUS_NACK);
    run_for(&f, INTERVAL_MS);

    HAC_CHECK_EQ_INT(f.reading.temperature_c.status, VAL_ERROR);
    HAC_CHECK_EQ_INT(f.reading.temperature_c.source_id, ENV_ID);
    HAC_CHECK_EQ_INT(slot(&f, 0)->sample.temperature_c.status, VAL_UNAVAILABLE);
    fixture_dispose(&f);
}

// --- requirements 15.1 -------------------------------------------------------

// "The device must not hang when a single sensor fails", and "a sensor error
// must be shown on screen": the BME68x stand-in stops answering for ten
// minutes in the middle of an hour.
static void requirement_15_1_one_failed_sensor_does_not_stop_the_device(void)
{
    fixture_t f;
    fixture_default(&f);
    run_until(&f, 20u * 60000u);
    HAC_CHECK_EQ_INT(f.reading.temperature_c.source_id, ENV_ID);

    fault_inject_set(&f.env, FAULT_ABSENT);  // unplugged
    run_for(&f, INTERVAL_MS);

    // Its own fields are errors at once; temperature and humidity come from
    // the fallback; CO2 never noticed.
    HAC_CHECK_EQ_INT(f.reading.pressure_hpa.status, VAL_ERROR);
    HAC_CHECK_EQ_INT(f.reading.pressure_hpa.source_id, ENV_ID);
    HAC_CHECK_EQ_INT(f.reading.gas_resistance_ohm.status, VAL_ERROR);
    HAC_CHECK_EQ_INT(f.reading.temperature_c.status, VAL_OK);
    HAC_CHECK_EQ_INT(f.reading.temperature_c.source_id, CO2_ID);
    HAC_CHECK_EQ_F32(f.reading.temperature_c.value, CO2_SOURCE_T_C, VALUE_EPS);
    HAC_CHECK_EQ_INT(f.reading.humidity_rh.source_id, CO2_ID);
    HAC_CHECK_EQ_INT(f.reading.co2_ppm.status, VAL_OK);

    const uint32_t co2_reads_before = slot(&f, 0)->reads;
    run_for(&f, 10u * 60000u);

    // Ten minutes later: CO2 kept its pace, the failed sensor was tried
    // again and again -- but not at the CO2 sensor's pace.
    HAC_CHECK_EQ_INT((int)(slot(&f, 0)->reads - co2_reads_before), 120);
    HAC_CHECK_EQ_U64(f.reading.co2_ppm.ts_ms, f.clock.now_ms);
    HAC_CHECK_EQ_INT(f.reading.pressure_hpa.status, VAL_ERROR);
    HAC_CHECK(f.env.calls.inits > 3u);
    HAC_CHECK(f.env.calls.inits < 12u);
    HAC_CHECK(!f.spun);

    fault_inject_clear(&f.env);  // plugged back
    run_for(&f, SENSOR_MANAGER_DEFAULT_BACKOFF_MAX_MS);

    // Found again without a restart, and the fields are its own again.
    HAC_CHECK_EQ_INT(f.reading.pressure_hpa.status, VAL_OK);
    HAC_CHECK_EQ_INT(f.reading.temperature_c.source_id, ENV_ID);
    HAC_CHECK_EQ_F32(f.reading.temperature_c.value, ENV_SOURCE_T_C, VALUE_EPS);
    HAC_CHECK(!slot(&f, 1)->failing);
    HAC_CHECK_EQ_INT((int)slot(&f, 1)->reinit_attempts, 0);
    fixture_dispose(&f);
}

static void a_failed_co2_sensor_shows_an_error_with_no_fallback(void)
{
    fixture_t f;
    fixture_default(&f);
    run_for(&f, 60000u);

    fault_inject_set(&f.co2, FAULT_BUS_NACK);
    run_for(&f, INTERVAL_MS);
    HAC_CHECK_EQ_INT(f.reading.co2_ppm.status, VAL_ERROR);
    HAC_CHECK_EQ_INT(f.reading.co2_ppm.source_id, CO2_ID);
    HAC_CHECK_EQ_INT(f.reading.temperature_c.status, VAL_OK);  // never came from it
    HAC_CHECK_EQ_INT(f.reading.temperature_c.source_id, ENV_ID);
    fixture_dispose(&f);
}

// --- failure and recovery ----------------------------------------------------

static void a_single_failed_read_is_retried_at_the_next_interval_not_reinitialised(void)
{
    fixture_t f;
    fixture_default(&f);
    run_for(&f, 30000u);

    fault_inject_set(&f.env, FAULT_BUS_TIMEOUT);
    run_for(&f, INTERVAL_MS);
    HAC_CHECK_EQ_INT(f.reading.pressure_hpa.status, VAL_ERROR);
    fault_inject_clear(&f.env);
    run_for(&f, INTERVAL_MS);

    HAC_CHECK_EQ_INT(f.reading.pressure_hpa.status, VAL_OK);
    HAC_CHECK_EQ_INT((int)f.env.calls.inits, 1);
    HAC_CHECK_EQ_INT((int)slot(&f, 1)->failures, 1);
    fixture_dispose(&f);
}

static void the_third_failure_in_a_row_starts_the_source_over(void)
{
    fixture_t f;
    fixture_default(&f);
    run_for(&f, 30000u);  // last read at 30 s

    fault_inject_set(&f.env, FAULT_BUS_NACK);
    run_until(&f, 45000u);  // reads at 35, 40, 45 s fail
    HAC_CHECK_EQ_INT((int)slot(&f, 1)->failures, 3);
    HAC_CHECK_EQ_INT(slot(&f, 1)->state, SENSOR_SLOT_BACKOFF);
    HAC_CHECK_EQ_INT((int)f.env.calls.inits, 1);

    run_until(&f, 45000u + SENSOR_MANAGER_DEFAULT_BACKOFF_FIRST_MS);
    HAC_CHECK_EQ_INT((int)f.env.calls.inits, 2);  // init() again, without deinit()
    fixture_dispose(&f);
}

static void an_absent_source_is_tried_at_a_doubling_backoff_up_to_the_ceiling(void)
{
    fixture_t f;
    fixture_default(&f);
    fault_inject_set(&f.env, FAULT_ABSENT);  // absent from boot

    // 0, then +5, +10, +20, +40, +80, +120, +120, ... seconds.
    const uint64_t attempts_at_s[] = {0u, 5u, 15u, 35u, 75u, 155u, 275u, 395u, 515u};
    for (size_t i = 0u; i < sizeof(attempts_at_s) / sizeof(attempts_at_s[0]); ++i) {
        const uint64_t at_ms = attempts_at_s[i] * 1000u;
        if (at_ms > 0u) {
            run_until(&f, at_ms - 1u);
            HAC_CHECK_EQ_INT((int)f.env.calls.inits, (int)i);  // not a moment early
        }
        run_until(&f, at_ms);
        HAC_CHECK_EQ_INT((int)f.env.calls.inits, (int)i + 1);
    }

    // From boot: its fields are errors, not "n/a" -- it was looked for.
    HAC_CHECK_EQ_INT(f.reading.pressure_hpa.status, VAL_ERROR);
    HAC_CHECK(!slot(&f, 1)->ever_ready);
    HAC_CHECK_EQ_INT((int)f.env.calls.reads, 0);  // never read what never initialised
    HAC_CHECK(!f.spun);
    fixture_dispose(&f);
}

static void a_source_that_initialises_but_never_reads_still_backs_off(void)
{
    fixture_t f;
    fixture_default(&f);
    fault_inject_set(&f.env, FAULT_BUS_NACK);  // init() works, every read fails
    run_until(&f, 3600000u);

    // Each round is an init() and three failed reads 5 s apart, then a
    // backoff that keeps doubling to the ceiling, since no read succeeds:
    // inits at 0, 15, 35, 65, 115 and 205 s, then every 130 s from 335 s to
    // 3585 s -- 6 + 26.
    HAC_CHECK_EQ_INT((int)f.env.calls.inits, 32);
    HAC_CHECK_EQ_INT((int)slot(&f, 1)->reads, 0);
    HAC_CHECK(!f.spun);
    fixture_dispose(&f);
}

// --- stalls and staleness ----------------------------------------------------

static void a_sensor_that_stops_delivering_goes_stale_then_error_then_is_started_over(void)
{
    fixture_t f;
    fixture_default(&f);
    run_for(&f, 60000u);  // last good read at 60 s

    fault_inject_set(&f.env, FAULT_SILENT);
    run_until(&f, 60000u + SENSOR_MANAGER_DEFAULT_MAX_AGE_MS - 1u);
    HAC_CHECK_EQ_INT(f.reading.pressure_hpa.status, VAL_OK);  // still fresh enough

    // Past the max age: stale, and the fallback takes temperature over.
    run_until(&f, 60000u + SENSOR_MANAGER_DEFAULT_MAX_AGE_MS + 1u);
    HAC_CHECK_EQ_INT(f.reading.pressure_hpa.status, VAL_STALE);
    HAC_CHECK_EQ_INT(f.reading.temperature_c.source_id, CO2_ID);

    // The cycle that began at 65 s has waited out the stall timeout: a
    // failure, and the field is an error now rather than an old number.
    run_until(&f, 65000u + SENSOR_MANAGER_DEFAULT_STALL_TIMEOUT_MS);
    HAC_CHECK_EQ_INT(f.reading.pressure_hpa.status, VAL_ERROR);
    HAC_CHECK_EQ_INT((int)slot(&f, 1)->failures, 1);

    // Three stalls in a row start it over.
    run_for(&f, 3u * (SENSOR_MANAGER_DEFAULT_STALL_TIMEOUT_MS + INTERVAL_MS));
    HAC_CHECK(f.env.calls.inits >= 2u);
    HAC_CHECK(!f.spun);
    fixture_dispose(&f);
}

static void a_stuck_sensor_ages_out_and_loses_to_the_fallback(void)
{
    fixture_t f;
    fixture_default(&f);
    run_for(&f, 60000u);

    // Answers every time, with the same old sample and its old timestamp.
    fault_inject_set(&f.env, FAULT_STUCK);
    run_for(&f, SENSOR_MANAGER_DEFAULT_MAX_AGE_MS + INTERVAL_MS);
    HAC_CHECK_EQ_INT(f.reading.pressure_hpa.status, VAL_STALE);
    HAC_CHECK_EQ_INT(f.reading.temperature_c.status, VAL_OK);
    HAC_CHECK_EQ_INT(f.reading.temperature_c.source_id, CO2_ID);
    fixture_dispose(&f);
}

// --- the merge's order of statuses -------------------------------------------

static void a_warming_primary_loses_to_a_working_fallback(void)
{
    fixture_t f;
    fixture_default(&f);
    fault_inject_set(&f.env, FAULT_FORCE_STATUS);
    fault_inject_set_forced_status(&f.env, VAL_WARMUP);
    poll(&f);

    HAC_CHECK_EQ_INT(f.reading.temperature_c.status, VAL_OK);
    HAC_CHECK_EQ_INT(f.reading.temperature_c.source_id, CO2_ID);
    HAC_CHECK_EQ_INT(f.reading.pressure_hpa.status, VAL_WARMUP);  // no fallback: shown as is
    fixture_dispose(&f);
}

static void a_warming_primary_beats_a_failed_fallback(void)
{
    fixture_t f;
    fixture_default(&f);
    fault_inject_set(&f.env, FAULT_FORCE_STATUS);
    fault_inject_set_forced_status(&f.env, VAL_WARMUP);
    fault_inject_set(&f.co2, FAULT_BUS_NACK);
    poll(&f);

    HAC_CHECK_EQ_INT(f.reading.temperature_c.status, VAL_WARMUP);
    HAC_CHECK_EQ_INT(f.reading.temperature_c.source_id, ENV_ID);
    fixture_dispose(&f);
}

static void with_both_failed_the_primary_error_is_shown(void)
{
    fixture_t f;
    fixture_default(&f);
    fault_inject_set(&f.env, FAULT_BUS_NACK);
    fault_inject_set(&f.co2, FAULT_BUS_NACK);
    poll(&f);

    HAC_CHECK_EQ_INT(f.reading.temperature_c.status, VAL_ERROR);
    HAC_CHECK_EQ_INT(f.reading.temperature_c.source_id, ENV_ID);
    fixture_dispose(&f);
}

// --- a poll takes time ----------------------------------------------------------

// A source whose read() waits on the clock first, as the drivers wait out
// their sensors' command times: the sample is stamped later than the moment
// the poll began.
typedef struct {
    sensor_source_t port;
    sensor_source_t *inner;
    const port_clock_t *clock;
} slow_source_t;

#define SLOW_READ_MS 50u

static sensor_source_t *slow_inner(const sensor_source_t *self)
{
    return ((const slow_source_t *)self->impl)->inner;
}

static hal_status_t slow_init(sensor_source_t *self)
{
    return slow_inner(self)->init(slow_inner(self));
}

static hal_status_t slow_start(sensor_source_t *self)
{
    return slow_inner(self)->start(slow_inner(self));
}

static uint32_t slow_ready_in_ms(const sensor_source_t *self)
{
    return slow_inner(self)->ready_in_ms(slow_inner(self));
}

static hal_status_t slow_read(sensor_source_t *self, sensor_sample_t *out)
{
    const slow_source_t *s = (const slow_source_t *)self->impl;
    s->clock->delay_ms(s->clock, SLOW_READ_MS);
    return s->inner->read(s->inner, out);
}

static hal_status_t slow_suspend(sensor_source_t *self)
{
    return slow_inner(self)->suspend(slow_inner(self));
}

static void slow_deinit(sensor_source_t *self)
{
    slow_inner(self)->deinit(slow_inner(self));
}

// Found in the simulator's fake-i2c mode: a loop that aged the reading
// against the time it read before the poll saw every fresh value stamped "in
// the future", of unknown age, and so stale. The merge must use the time
// after the poll, and so must whoever ages the reading afterwards.
static void a_value_read_during_a_slow_poll_is_fresh_not_of_unknown_age(void)
{
    fixture_t f;
    fixture_default(&f);

    slow_source_t slow = {
        .port = f.env.port,
        .inner = &f.env.port,
        .clock = fake_clock_port(&f.clock),
    };
    slow.port.init = slow_init;
    slow.port.start = slow_start;
    slow.port.ready_in_ms = slow_ready_in_ms;
    slow.port.read = slow_read;
    slow.port.suspend = slow_suspend;
    slow.port.deinit = slow_deinit;
    slow.port.impl = &slow;
    f.mgr.slots[1].source = &slow.port;

    const uint64_t poll_began = f.clock.now_ms;
    poll(&f);
    HAC_CHECK(f.reading.pressure_hpa.ts_ms > poll_began);
    HAC_CHECK_EQ_INT(f.reading.pressure_hpa.status, VAL_OK);

    air_reading_apply_age(&f.reading, f.clock.now_ms, SENSOR_MANAGER_DEFAULT_MAX_AGE_MS);
    HAC_CHECK_EQ_INT(f.reading.pressure_hpa.status, VAL_OK);
    fixture_dispose(&f);
}

// --- ambient compensation (docs/adr/0018) ------------------------------------

// Wraps a source and gives it a set_ambient() that records what it was
// handed -- standing in for the SCD4x, whose own set_ambient() is checked
// against the SCD41 model in tests/contract/test_port_sensor_contract.c.
typedef struct {
    sensor_source_t port;
    sensor_source_t *inner;
    uint32_t calls;
    float last_hpa;
    hal_status_t answer;
} ambient_recorder_t;

static sensor_source_t *inner_of(const sensor_source_t *self)
{
    return ((const ambient_recorder_t *)self->impl)->inner;
}

static hal_status_t rec_init(sensor_source_t *self)
{
    return inner_of(self)->init(inner_of(self));
}

static hal_status_t rec_start(sensor_source_t *self)
{
    return inner_of(self)->start(inner_of(self));
}

static uint32_t rec_ready_in_ms(const sensor_source_t *self)
{
    return inner_of(self)->ready_in_ms(inner_of(self));
}

static hal_status_t rec_read(sensor_source_t *self, sensor_sample_t *out)
{
    return inner_of(self)->read(inner_of(self), out);
}

static hal_status_t rec_suspend(sensor_source_t *self)
{
    return inner_of(self)->suspend(inner_of(self));
}

static void rec_deinit(sensor_source_t *self)
{
    inner_of(self)->deinit(inner_of(self));
}

static hal_status_t rec_set_ambient(sensor_source_t *self, const sensor_ambient_t *ambient)
{
    ambient_recorder_t *r = (ambient_recorder_t *)self->impl;
    r->calls++;
    r->last_hpa = ambient->pressure_hpa.value;
    HAC_CHECK_EQ_INT(ambient->pressure_hpa.status, VAL_OK);  // only what was measured
    return r->answer;
}

// A fixture whose CO2 stand-in takes the pressure, and whose BME68x stand-in
// reports a steady 1000 hPa, or one that climbs 10 hPa over 100 s and then
// holds.
typedef struct {
    fixture_t f;
    ambient_recorder_t rec;
} ambient_fixture_t;

static void ambient_fixture_init(ambient_fixture_t *a, bool climbing)
{
    fixture_t *f = &a->f;
    fixture_default(f);

    replay_source_deinit(&f->env_replay);
    char *env_path =
        climbing ? write_temp_csv(
                       "mgr_env_climbing.csv",
                       "t_offset_s,temperature_c,humidity_rh,pressure_hpa,gas_res_ohm,status\n"
                       "0,21.0,50.0,1000.0,100000,ok\n"
                       "100,21.0,50.0,1010.0,100000,ok\n"
                       "3600,21.0,50.0,1010.0,100000,ok\n")
                 : write_temp_csv(
                       "mgr_env_steady.csv",
                       "t_offset_s,temperature_c,humidity_rh,pressure_hpa,gas_res_ohm,status\n"
                       "0,21.0,50.0,1000.0,100000,ok\n"
                       "3600,21.0,50.0,1000.0,100000,ok\n");
    HAC_CHECK(env_path != NULL);
    HAC_CHECK_EQ_INT(replay_source_init(&f->env_replay, env_path,
                                        SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY |
                                            SENS_CAP_PRESSURE | SENS_CAP_GAS_RES,
                                        ENV_ID, fake_clock_port(&f->clock), true),
                     HAL_OK);
    fault_inject_init(&f->env, &f->env_replay.port);

    memset(&a->rec, 0, sizeof(a->rec));
    a->rec.inner = &f->co2.port;
    a->rec.answer = HAL_OK;
    a->rec.port = f->co2.port;
    a->rec.port.init = rec_init;
    a->rec.port.start = rec_start;
    a->rec.port.ready_in_ms = rec_ready_in_ms;
    a->rec.port.read = rec_read;
    a->rec.port.suspend = rec_suspend;
    a->rec.port.deinit = rec_deinit;
    a->rec.port.set_ambient = rec_set_ambient;
    a->rec.port.impl = &a->rec;

    // The same manager, with the recorder in the CO2 stand-in's place.
    const sensor_manager_config_t cfg = sensor_manager_config_default();
    sensor_manager_init(&f->mgr, fake_clock_port(&f->clock), NULL, &cfg);
    HAC_CHECK_EQ_INT(sensor_manager_add(&f->mgr, &a->rec.port, INTERVAL_MS), HAL_OK);
    HAC_CHECK_EQ_INT(sensor_manager_add(&f->mgr, &f->env.port, INTERVAL_MS), HAL_OK);
    set_board_priorities(&f->mgr);
}

static void the_pressure_reaches_a_compensating_source_at_once_then_only_on_change(void)
{
    ambient_fixture_t a;
    ambient_fixture_init(&a, true);

    poll(&a.f);
    HAC_CHECK_EQ_INT((int)a.rec.calls, 1);  // the first poll that measured a pressure
    HAC_CHECK_EQ_F32(a.rec.last_hpa, 1000.0f, VALUE_EPS);

    // Climbing 0.5 hPa per 5 s read: handed over every second or third read,
    // never for less than a hectopascal.
    run_until(&a.f, 100000u);
    HAC_CHECK(a.rec.calls >= 6u);
    HAC_CHECK(a.rec.calls <= 11u);
    HAC_CHECK(a.rec.last_hpa >= 1009.0f - VALUE_EPS);

    const uint32_t settled = a.rec.calls;
    run_until(&a.f, 600000u);
    HAC_CHECK(a.rec.calls <= settled + 1u);  // at most the last step to 1010
    HAC_CHECK_EQ_F32(a.rec.last_hpa, 1010.0f, 0.5f);
    fixture_dispose(&a.f);
}

static void a_compensating_source_gets_the_pressure_again_after_it_is_started_over(void)
{
    ambient_fixture_t a;
    ambient_fixture_init(&a, false);
    run_until(&a.f, 200000u);
    const uint32_t before = a.rec.calls;

    // Its init() restores the sensor's own default, so the unchanged
    // pressure must be handed over again once it is back.
    fault_inject_set(&a.f.co2, FAULT_BUS_NACK);
    run_for(&a.f, 3u * INTERVAL_MS);
    HAC_CHECK_EQ_INT(sensor_manager_slot(&a.f.mgr, 0)->state, SENSOR_SLOT_BACKOFF);
    fault_inject_clear(&a.f.co2);
    run_for(&a.f, SENSOR_MANAGER_DEFAULT_BACKOFF_FIRST_MS);

    HAC_CHECK_EQ_INT((int)a.f.co2.calls.inits, 2);
    HAC_CHECK_EQ_INT((int)a.rec.calls, (int)before + 1);
    fixture_dispose(&a.f);
}

static void a_busy_answer_is_asked_again_and_a_refused_value_is_not(void)
{
    ambient_fixture_t a;
    ambient_fixture_init(&a, false);

    a.rec.answer = HAL_ERR_BUSY;
    poll(&a.f);
    HAC_CHECK_EQ_INT((int)a.rec.calls, 1);
    run_for(&a.f, INTERVAL_MS);
    HAC_CHECK(a.rec.calls > 1u);  // asked again at the next poll
    HAC_CHECK(!sensor_manager_slot(&a.f.mgr, 0)->ambient_sent);

    a.rec.answer = HAL_ERR_INVALID_ARG;
    run_for(&a.f, INTERVAL_MS);
    HAC_CHECK(sensor_manager_slot(&a.f.mgr, 0)->ambient_sent);
    const uint32_t refused = a.rec.calls;
    run_for(&a.f, 60000u);
    HAC_CHECK_EQ_INT((int)a.rec.calls, (int)refused);  // the same value is not offered again
    fixture_dispose(&a.f);
}

static void nothing_is_handed_over_while_the_pressure_is_not_measured(void)
{
    ambient_fixture_t a;
    ambient_fixture_init(&a, false);
    fault_inject_set(&a.f.env, FAULT_ABSENT);
    run_until(&a.f, 60000u);

    HAC_CHECK_EQ_INT(a.f.reading.pressure_hpa.status, VAL_ERROR);
    HAC_CHECK_EQ_INT((int)a.rec.calls, 0);
    fixture_dispose(&a.f);
}

int main(void)
{
    HAC_RUN(add_refuses_what_it_could_not_tell_apart);
    HAC_RUN(an_empty_manager_has_no_deadline_and_touches_nothing);
    HAC_RUN(the_first_poll_initialises_starts_and_reads_every_source);
    HAC_RUN(each_source_is_read_once_per_interval);
    HAC_RUN(a_field_without_priority_takes_the_sources_in_the_order_added);
    HAC_RUN(a_priority_may_name_a_source_that_does_not_exist);
    HAC_RUN(a_source_cannot_fill_a_field_it_does_not_claim);
    HAC_RUN(requirement_15_1_one_failed_sensor_does_not_stop_the_device);
    HAC_RUN(a_failed_co2_sensor_shows_an_error_with_no_fallback);
    HAC_RUN(a_single_failed_read_is_retried_at_the_next_interval_not_reinitialised);
    HAC_RUN(the_third_failure_in_a_row_starts_the_source_over);
    HAC_RUN(an_absent_source_is_tried_at_a_doubling_backoff_up_to_the_ceiling);
    HAC_RUN(a_source_that_initialises_but_never_reads_still_backs_off);
    HAC_RUN(a_sensor_that_stops_delivering_goes_stale_then_error_then_is_started_over);
    HAC_RUN(a_stuck_sensor_ages_out_and_loses_to_the_fallback);
    HAC_RUN(a_warming_primary_loses_to_a_working_fallback);
    HAC_RUN(a_warming_primary_beats_a_failed_fallback);
    HAC_RUN(with_both_failed_the_primary_error_is_shown);
    HAC_RUN(a_value_read_during_a_slow_poll_is_fresh_not_of_unknown_age);
    HAC_RUN(the_pressure_reaches_a_compensating_source_at_once_then_only_on_change);
    HAC_RUN(a_compensating_source_gets_the_pressure_again_after_it_is_started_over);
    HAC_RUN(a_busy_answer_is_asked_again_and_a_refused_value_is_not);
    HAC_RUN(nothing_is_handed_over_while_the_pressure_is_not_measured);
    return hac_test_summary();
}
