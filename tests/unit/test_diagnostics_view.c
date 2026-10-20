// SPDX-License-Identifier: Apache-2.0
//
// The diagnostics screen's view model (core/ui/diagnostics_view.h): each
// source's state, failures and restarts; each reading's status, accuracy and
// source; the clock and the recorder -- and diag_view_equal(), which decides
// whether the screen redraws, ignoring what changes at every poll.

#include <stdint.h>
#include <string.h>

#include "app/sensor_manager.h"
#include "hac_test.h"
#include "ui/diagnostics_view.h"

static sensor_manager_t g_mgr;
static sensor_source_t g_scd = {.name = "scd4x", .id = 1u};
static sensor_source_t g_bme = {.name = "bme68x", .id = 2u};
static air_reading_t g_r;

static void setup(void)
{
    memset(&g_mgr, 0, sizeof(g_mgr));
    g_mgr.count = 2u;
    g_mgr.slots[0].source = &g_scd;
    g_mgr.slots[1].source = &g_bme;
    for (size_t i = 0u; i < 2u; ++i) {
        g_mgr.slots[i].state = SENSOR_SLOT_IDLE;
        g_mgr.slots[i].ever_ready = true;
        g_mgr.slots[i].inits = 1u;
    }
    air_reading_init(&g_r);
    g_r.co2_ppm = measurement_ok(700.0f, 100u, 1u);
    g_r.temperature_c = measurement_ok(22.0f, 100u, 2u);
    g_r.humidity_rh = measurement_ok(45.0f, 100u, 2u);
    g_r.pressure_hpa = measurement_ok(1006.0f, 100u, 2u);
    g_r.gas_resistance_ohm = measurement_ok(150000.0f, 100u, 2u);
    g_r.wall_time_ms = 1792148862000u;
}

static diag_view_t build(const char *clock_source, bool recorder, recorder_state_t state)
{
    const diag_facts_t f = {
        .reading = &g_r,
        .sensors = &g_mgr,
        .recorder = {.present = recorder, .state = state},
        .clock_source = clock_source,
    };
    diag_view_t v;
    diag_view_build(&v, &f);
    return v;
}

static void every_source_with_its_state_and_counts(void)
{
    setup();
    g_mgr.slots[1].failing = true;
    g_mgr.slots[1].failures = 7u;
    g_mgr.slots[1].inits = 3u;
    const diag_view_t v = build("rtc", true, RECORDER_ON);
    HAC_CHECK_EQ_INT(v.slot_count, 2);
    HAC_CHECK(strcmp(v.slots[0].name, "scd4x") == 0);
    HAC_CHECK(strcmp(v.slots[0].state, "running") == 0);
    HAC_CHECK(!v.slots[0].trouble);
    HAC_CHECK(strcmp(v.slots[0].failures, "0") == 0);
    HAC_CHECK(strcmp(v.slots[0].restarts, "0") == 0);
    HAC_CHECK(strcmp(v.slots[1].state, "failing") == 0);
    HAC_CHECK(v.slots[1].trouble);
    HAC_CHECK(strcmp(v.slots[1].failures, "7") == 0);
    HAC_CHECK(strcmp(v.slots[1].restarts, "2") == 0);  // three inits: the start and two restarts
}

static void the_slot_states_fold_what_changes_every_cycle(void)
{
    setup();
    sensor_manager_slot_t *s = &g_mgr.slots[0];
    s->state = SENSOR_SLOT_UNINIT;
    s->ever_ready = false;
    HAC_CHECK(strcmp(diag_view_slot_state(s), "starting") == 0);
    s->state = SENSOR_SLOT_IDLE;
    HAC_CHECK(strcmp(diag_view_slot_state(s), "starting") == 0);  // init() never succeeded yet
    s->ever_ready = true;
    HAC_CHECK(strcmp(diag_view_slot_state(s), "running") == 0);
    s->state = SENSOR_SLOT_MEASURING;
    HAC_CHECK(strcmp(diag_view_slot_state(s), "running") == 0);  // the same word: no redraw
    s->state = SENSOR_SLOT_BACKOFF;
    HAC_CHECK(strcmp(diag_view_slot_state(s), "restarting") == 0);
}

static void counts_stop_at_999_plus(void)
{
    setup();
    g_mgr.slots[0].failures = 999u;
    g_mgr.slots[1].failures = 1000u;
    g_mgr.slots[1].inits = 4000000000u;
    const diag_view_t v = build(NULL, false, RECORDER_WAITING);
    HAC_CHECK(strcmp(v.slots[0].failures, "999") == 0);
    HAC_CHECK(strcmp(v.slots[1].failures, "999+") == 0);
    HAC_CHECK(strcmp(v.slots[1].restarts, "999+") == 0);
}

static void every_reading_with_status_accuracy_and_source(void)
{
    setup();
    // The fallback at work: the BME68x down, temperature from the SCD41.
    g_r.temperature_c = measurement_ok(22.4f, 100u, 1u);
    g_r.humidity_rh = measurement_status(VAL_ERROR, 100u, 2u);
    g_r.iaq = measurement_ok(55.0f, 100u, 2u);
    g_r.iaq.accuracy = ACC_LOW;
    g_r.pressure_hpa = measurement_status(VAL_STALE, 100u, 2u);
    const diag_view_t v = build(NULL, false, RECORDER_WAITING);

    const diag_field_view_t *f = v.fields;
    HAC_CHECK(strcmp(f[DIAG_FIELD_CO2].label, "CO2") == 0);
    HAC_CHECK(strcmp(f[DIAG_FIELD_CO2].status, "ok") == 0);
    HAC_CHECK(strcmp(f[DIAG_FIELD_CO2].accuracy, "-") == 0);
    HAC_CHECK(strcmp(f[DIAG_FIELD_CO2].source, "scd4x") == 0);
    HAC_CHECK(strcmp(f[DIAG_FIELD_TEMPERATURE].source, "scd4x") == 0);
    HAC_CHECK(strcmp(f[DIAG_FIELD_HUMIDITY].status, "ERR") == 0);
    HAC_CHECK(f[DIAG_FIELD_HUMIDITY].trouble);
    HAC_CHECK(strcmp(f[DIAG_FIELD_PRESSURE].status, "stale") == 0);
    HAC_CHECK(!f[DIAG_FIELD_PRESSURE].trouble);
    HAC_CHECK(strcmp(f[DIAG_FIELD_IAQ].accuracy, "low") == 0);
    HAC_CHECK(f[DIAG_FIELD_IAQ].trouble);  // a number its source has not calibrated
    HAC_CHECK(strcmp(f[DIAG_FIELD_GAS].label, "Gas") == 0);

    // Nothing provides it: no source.
    g_r.iaq = measurement_unavailable();
    const diag_view_t w = build(NULL, false, RECORDER_WAITING);
    HAC_CHECK(strcmp(w.fields[DIAG_FIELD_IAQ].status, "n/a") == 0);
    HAC_CHECK(strcmp(w.fields[DIAG_FIELD_IAQ].source, "") == 0);
    // A source id no slot has.
    g_r.co2_ppm.source_id = 9u;
    const diag_view_t u = build(NULL, false, RECORDER_WAITING);
    HAC_CHECK(strcmp(u.fields[DIAG_FIELD_CO2].source, "") == 0);
}

static void the_clock_and_the_recorder(void)
{
    setup();
    diag_view_t v = build("rtc", true, RECORDER_ON);
    HAC_CHECK(v.clock_set);
    HAC_CHECK(strcmp(v.clock_text, "Clock: set (rtc)") == 0);
    HAC_CHECK(strcmp(v.recorder_text, "Recording: on") == 0);
    HAC_CHECK(!v.recorder_trouble);

    v = build(NULL, true, RECORDER_NO_MEDIUM);
    HAC_CHECK(strcmp(v.clock_text, "Clock: set") == 0);
    HAC_CHECK(strcmp(v.recorder_text, "Recording: no medium") == 0);
    HAC_CHECK(v.recorder_trouble);

    g_r.wall_time_ms = 0u;
    v = build("rtc", false, RECORDER_ON);
    HAC_CHECK(!v.clock_set);
    HAC_CHECK(strcmp(v.clock_text, "Clock: not set") == 0);  // no source for a clock not set
    HAC_CHECK(strcmp(v.recorder_text, "Recording: off") == 0);

    v = build(NULL, true, RECORDER_FULL);
    HAC_CHECK(strcmp(v.recorder_text, "Recording: full") == 0);
    v = build(NULL, true, RECORDER_FAILED);
    HAC_CHECK(strcmp(v.recorder_text, "Recording: failed") == 0);
    v = build(NULL, true, RECORDER_WAITING);
    HAC_CHECK(strcmp(v.recorder_text, "Recording: waiting") == 0);
}

static void equal_ignores_what_changes_every_poll(void)
{
    setup();
    const diag_view_t a = build("rtc", true, RECORDER_ON);
    // A new reading, its time, the wall clock moving, a read counted, the
    // slot measuring: the same picture.
    g_r.co2_ppm = measurement_ok(735.0f, 5100u, 1u);
    g_r.wall_time_ms += 5000u;
    g_mgr.slots[0].reads = 200u;
    g_mgr.slots[0].state = SENSOR_SLOT_MEASURING;
    const diag_view_t b = build("rtc", true, RECORDER_ON);
    HAC_CHECK(diag_view_equal(&a, &b));

    // A failure is not.
    g_mgr.slots[0].failures = 1u;
    const diag_view_t c = build("rtc", true, RECORDER_ON);
    HAC_CHECK(!diag_view_equal(&a, &c));
    // Nor a status, an accuracy, the recorder, the clock.
    setup();
    g_r.iaq = measurement_ok(55.0f, 100u, 2u);
    g_r.iaq.accuracy = ACC_MEDIUM;
    const diag_view_t d = build("rtc", true, RECORDER_ON);
    g_r.iaq.accuracy = ACC_HIGH;
    const diag_view_t e = build("rtc", true, RECORDER_ON);
    HAC_CHECK(!diag_view_equal(&d, &e));
    HAC_CHECK(!diag_view_equal(&d, &(diag_view_t){0}));
    const diag_view_t g = build("rtc", true, RECORDER_FULL);
    HAC_CHECK(!diag_view_equal(&e, &g));
    const diag_view_t h = build("console", true, RECORDER_ON);
    HAC_CHECK(!diag_view_equal(&e, &h));
}

static void without_a_sensor_manager_there_are_no_sources(void)
{
    setup();
    const diag_facts_t f = {.reading = &g_r, .sensors = NULL};
    diag_view_t v;
    diag_view_build(&v, &f);
    HAC_CHECK_EQ_INT(v.slot_count, 0);
    HAC_CHECK(strcmp(v.fields[DIAG_FIELD_CO2].source, "") == 0);
}

int main(void)
{
    HAC_RUN(every_source_with_its_state_and_counts);
    HAC_RUN(the_slot_states_fold_what_changes_every_cycle);
    HAC_RUN(counts_stop_at_999_plus);
    HAC_RUN(every_reading_with_status_accuracy_and_source);
    HAC_RUN(the_clock_and_the_recorder);
    HAC_RUN(equal_ignores_what_changes_every_poll);
    HAC_RUN(without_a_sensor_manager_there_are_no_sources);
    return hac_test_summary();
}
