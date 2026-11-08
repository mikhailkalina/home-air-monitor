// SPDX-License-Identifier: Apache-2.0
//
// core/app/battery_gauge.h: the table, to its ends and between its points; a
// median that sags do not reach; a number that moves in steps and still reaches
// either end; what is not a cell's voltage, with its hysteresis; refused bursts
// keeping the number until the silence; a burst's cadence; and a whole discharge
// with sags in it, whose number only falls
// (docs/adr/0040-a-battery-gauge-from-the-voltage.md).

#include <stdint.h>
#include <string.h>

#include "app/battery_gauge.h"
#include "hac_test.h"

#define LOWEST_CELL_MV 2500u
#define HIGHEST_CELL_MV 4400u
#define INTERVAL BATTERY_GAUGE_DEFAULT_SAMPLE_INTERVAL_MS
#define SOURCE_ID 3u

// Round numbers, so that a test can say what lies between two points; the
// board's table is the board's (platform/esp32_t5s3/board_battery.c).
static const battery_curve_point_t k_curve[] = {
    {3300u, 0u},
    {3700u, 20u},
    {3800u, 50u},
    {4200u, 100u},
};
#define CURVE_POINTS (sizeof(k_curve) / sizeof(k_curve[0]))

static battery_gauge_config_t config(void)
{
    battery_gauge_config_t cfg = battery_gauge_config_default();
    cfg.curve = k_curve;
    cfg.curve_points = CURVE_POINTS;
    cfg.lowest_cell_mv = LOWEST_CELL_MV;
    cfg.highest_cell_mv = HIGHEST_CELL_MV;
    cfg.source_id = SOURCE_ID;
    return cfg;
}

static battery_gauge_t fresh(void)
{
    battery_gauge_t g;
    const battery_gauge_config_t cfg = config();
    battery_gauge_init(&g, &cfg);
    return g;
}

// A burst at each of `count` moments, INTERVAL apart, all of `mv`. Returns
// when the next would be.
static uint64_t feed(battery_gauge_t *g, uint64_t t, uint32_t mv, size_t count)
{
    for (size_t i = 0u; i < count; ++i) {
        battery_gauge_sample(g, true, mv, t);
        t += INTERVAL;
    }
    return t;
}

static float shown(const battery_gauge_t *g, uint64_t t)
{
    measurement_f32_t percent;
    measurement_f32_t mv;
    battery_gauge_fill(g, t, &percent, &mv);
    HAC_CHECK_EQ_INT(percent.status, VAL_OK);
    return percent.value;
}

static void the_table_holds_at_its_points_and_past_its_ends(void)
{
    HAC_CHECK_EQ_INT(battery_percent_from_mv(k_curve, CURVE_POINTS, 0u), 0);
    HAC_CHECK_EQ_INT(battery_percent_from_mv(k_curve, CURVE_POINTS, 3299u), 0);
    for (size_t i = 0u; i < CURVE_POINTS; ++i) {
        HAC_CHECK_EQ_INT(battery_percent_from_mv(k_curve, CURVE_POINTS, k_curve[i].mv),
                         k_curve[i].percent);
    }
    HAC_CHECK_EQ_INT(battery_percent_from_mv(k_curve, CURVE_POINTS, 4201u), 100);
    HAC_CHECK_EQ_INT(battery_percent_from_mv(k_curve, CURVE_POINTS, 65535u), 100);
    HAC_CHECK_EQ_INT(battery_percent_from_mv(NULL, 0u, 3700u), 0);
    HAC_CHECK_EQ_INT(battery_percent_from_mv(k_curve, 0u, 3700u), 0);
}

static void between_two_points_it_is_linear_and_rounds_to_nearest(void)
{
    HAC_CHECK_EQ_INT(battery_percent_from_mv(k_curve, CURVE_POINTS, 3750u), 35);
    HAC_CHECK_EQ_INT(battery_percent_from_mv(k_curve, CURVE_POINTS, 3500u), 10);
    // 20 % over 400 mV: 1 % per 20 mV, and half of it rounds up.
    HAC_CHECK_EQ_INT(battery_percent_from_mv(k_curve, CURVE_POINTS, 3309u), 0);
    HAC_CHECK_EQ_INT(battery_percent_from_mv(k_curve, CURVE_POINTS, 3310u), 1);
    uint8_t last = 0u;
    for (uint32_t mv = 3200u; mv <= 4300u; ++mv) {
        const uint8_t p = battery_percent_from_mv(k_curve, CURVE_POINTS, mv);
        hac_test_check(p >= last, "the percentage never falls as the voltage rises", __FILE__,
                       __LINE__);
        last = p;
    }
}

static void nothing_is_known_before_the_first_burst(void)
{
    const battery_gauge_t g = fresh();
    measurement_f32_t percent;
    measurement_f32_t mv;
    battery_gauge_fill(&g, 1000u, &percent, &mv);
    HAC_CHECK_EQ_INT(percent.status, VAL_UNAVAILABLE);
    HAC_CHECK_EQ_INT(mv.status, VAL_UNAVAILABLE);
    HAC_CHECK_EQ_INT(battery_gauge_state(&g), BATTERY_GAUGE_NO_SAMPLE);
    HAC_CHECK(battery_gauge_due(&g, 0u));
}

static void the_first_burst_is_the_first_number(void)
{
    battery_gauge_t g = fresh();
    battery_gauge_sample(&g, true, 3750u, 4000u);
    measurement_f32_t percent;
    measurement_f32_t mv;
    battery_gauge_fill(&g, 4100u, &percent, &mv);
    HAC_CHECK_EQ_INT(percent.status, VAL_OK);
    HAC_CHECK_EQ_F32(percent.value, 35.0f, 0.0f);
    HAC_CHECK_EQ_U64(percent.ts_ms, 4100u);  // the time it is read at: never ages
    HAC_CHECK_EQ_INT(percent.source_id, SOURCE_ID);
    HAC_CHECK_EQ_INT(mv.status, VAL_OK);
    HAC_CHECK_EQ_F32(mv.value, 3750.0f, 0.0f);
    HAC_CHECK_EQ_INT(battery_gauge_state(&g), BATTERY_GAUGE_CELL);
}

static void two_sags_of_five_do_not_reach_the_number(void)
{
    battery_gauge_t g = fresh();
    uint64_t t = feed(&g, 0u, 3800u, 5u);
    HAC_CHECK_EQ_F32(shown(&g, t), 50.0f, 0.0f);
    t = feed(&g, t, 3600u, 2u);  // a full refresh, a radio's start
    HAC_CHECK_EQ_F32(shown(&g, t), 50.0f, 0.0f);
    t = feed(&g, t, 3800u, 3u);
    HAC_CHECK_EQ_F32(shown(&g, t), 50.0f, 0.0f);
    // Three of five are no sag: the cell is there.
    t = feed(&g, t, 3700u, 3u);
    HAC_CHECK_EQ_F32(shown(&g, t), 20.0f, 0.0f);
}

static void the_number_moves_in_steps(void)
{
    battery_gauge_t g = fresh();
    uint64_t t = feed(&g, 0u, 3800u, 5u);  // 50 %
    HAC_CHECK_EQ_F32(shown(&g, t), 50.0f, 0.0f);
    // 49 %, one point away (3.3 mV a point below 3800): stays.
    t = feed(&g, t, 3796u, 5u);
    HAC_CHECK_EQ_F32(shown(&g, t), 50.0f, 0.0f);
    // BATTERY_GAUGE_DEFAULT_PERCENT_STEP away, exactly: moves.
    t = feed(&g, t, 3792u, 5u);
    HAC_CHECK_EQ_F32(shown(&g, t), 48.0f, 0.0f);
    // And back the same way: one point up stays, two moves.
    t = feed(&g, t, 3796u, 5u);
    HAC_CHECK_EQ_F32(shown(&g, t), 48.0f, 0.0f);
    t = feed(&g, t, 3800u, 5u);
    HAC_CHECK_EQ_F32(shown(&g, t), 50.0f, 0.0f);
}

static void either_end_is_reached_whatever_the_step(void)
{
    battery_gauge_t g = fresh();
    uint64_t t = feed(&g, 0u, 4192u, 5u);  // 99 %
    HAC_CHECK_EQ_F32(shown(&g, t), 99.0f, 0.0f);
    t = feed(&g, t, 4200u, 5u);
    HAC_CHECK_EQ_F32(shown(&g, t), 100.0f, 0.0f);

    t = feed(&g, t, 3320u, 5u);  // 1 %
    HAC_CHECK_EQ_F32(shown(&g, t), 1.0f, 0.0f);
    t = feed(&g, t, 3300u, 5u);
    HAC_CHECK_EQ_F32(shown(&g, t), 0.0f, 0.0f);
}

static void above_a_cell_the_voltage_is_the_supplys(void)
{
    battery_gauge_t g = fresh();
    uint64_t t = feed(&g, 0u, 4650u, 1u);
    measurement_f32_t percent;
    measurement_f32_t mv;
    battery_gauge_fill(&g, t, &percent, &mv);
    HAC_CHECK_EQ_INT(battery_gauge_state(&g), BATTERY_GAUGE_NOT_A_CELL);
    HAC_CHECK_EQ_INT(percent.status, VAL_UNAVAILABLE);
    HAC_CHECK_EQ_INT(mv.status, VAL_OK);  // real, and what the log is to show
    HAC_CHECK_EQ_F32(mv.value, 4650.0f, 0.0f);
}

static void the_limits_are_exact_for_the_first_burst(void)
{
    const uint32_t cases[][2] = {
        {HIGHEST_CELL_MV, BATTERY_GAUGE_CELL},
        {HIGHEST_CELL_MV + 1u, BATTERY_GAUGE_NOT_A_CELL},
        {LOWEST_CELL_MV, BATTERY_GAUGE_CELL},
        {LOWEST_CELL_MV - 1u, BATTERY_GAUGE_NOT_A_CELL},
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        battery_gauge_t g = fresh();
        battery_gauge_sample(&g, true, cases[i][0], 0u);
        HAC_CHECK_EQ_INT(battery_gauge_state(&g), (int)cases[i][1]);
    }
}

static void coming_back_inside_takes_the_hysteresis(void)
{
    battery_gauge_t g = fresh();
    uint64_t t = feed(&g, 0u, HIGHEST_CELL_MV + 200u, 5u);
    HAC_CHECK_EQ_INT(battery_gauge_state(&g), BATTERY_GAUGE_NOT_A_CELL);
    const uint32_t hysteresis = BATTERY_GAUGE_DEFAULT_LIMIT_HYSTERESIS_MV;
    t = feed(&g, t, HIGHEST_CELL_MV - hysteresis + 1u, 5u);
    HAC_CHECK_EQ_INT(battery_gauge_state(&g), BATTERY_GAUGE_NOT_A_CELL);
    t = feed(&g, t, HIGHEST_CELL_MV - hysteresis, 5u);
    HAC_CHECK_EQ_INT(battery_gauge_state(&g), BATTERY_GAUGE_CELL);
    // Inside again, the limit itself is still a cell's.
    t = feed(&g, t, HIGHEST_CELL_MV, 5u);
    HAC_CHECK_EQ_INT(battery_gauge_state(&g), BATTERY_GAUGE_CELL);

    t = feed(&g, t, LOWEST_CELL_MV - 200u, 5u);
    HAC_CHECK_EQ_INT(battery_gauge_state(&g), BATTERY_GAUGE_NOT_A_CELL);
    t = feed(&g, t, LOWEST_CELL_MV + hysteresis - 1u, 5u);
    HAC_CHECK_EQ_INT(battery_gauge_state(&g), BATTERY_GAUGE_NOT_A_CELL);
    t = feed(&g, t, LOWEST_CELL_MV + hysteresis, 5u);
    HAC_CHECK_EQ_INT(battery_gauge_state(&g), BATTERY_GAUGE_CELL);
}

// A cell's first number after USB went: as it is, not stepped towards.
static void a_new_cell_takes_its_number_at_once(void)
{
    battery_gauge_t g = fresh();
    uint64_t t = feed(&g, 0u, 3800u, 5u);
    HAC_CHECK_EQ_F32(shown(&g, t), 50.0f, 0.0f);
    t = feed(&g, t, 4650u, 5u);
    HAC_CHECK_EQ_INT(battery_gauge_state(&g), BATTERY_GAUGE_NOT_A_CELL);
    t = feed(&g, t, 3796u, 5u);
    HAC_CHECK_EQ_F32(shown(&g, t), 49.0f, 0.0f);
}

static void refused_bursts_keep_the_number_until_the_silence(void)
{
    battery_gauge_t g = fresh();
    uint64_t t = feed(&g, 0u, 3800u, 1u);
    const uint64_t last_good = t - INTERVAL;
    const uint64_t silence_ends = last_good + BATTERY_GAUGE_DEFAULT_MAX_SILENCE_MS;

    battery_gauge_sample(&g, false, 0u, silence_ends - 1u);
    HAC_CHECK_EQ_F32(shown(&g, silence_ends), 50.0f, 0.0f);
    battery_gauge_sample(&g, false, 12345u, silence_ends);
    HAC_CHECK_EQ_INT(battery_gauge_state(&g), BATTERY_GAUGE_SILENT);
    measurement_f32_t percent;
    measurement_f32_t mv;
    battery_gauge_fill(&g, silence_ends, &percent, &mv);
    HAC_CHECK_EQ_INT(percent.status, VAL_ERROR);
    HAC_CHECK_EQ_INT(mv.status, VAL_ERROR);

    battery_gauge_sample(&g, true, 3800u, silence_ends + INTERVAL);
    HAC_CHECK_EQ_INT(battery_gauge_state(&g), BATTERY_GAUGE_CELL);
    HAC_CHECK_EQ_F32(shown(&g, silence_ends + INTERVAL), 50.0f, 0.0f);
}

static void an_adc_that_never_answers_is_an_error_in_time(void)
{
    battery_gauge_t g = fresh();
    const uint64_t start = 7000u;
    battery_gauge_sample(&g, false, 0u, start);
    HAC_CHECK_EQ_INT(battery_gauge_state(&g), BATTERY_GAUGE_NO_SAMPLE);
    battery_gauge_sample(&g, false, 0u, start + BATTERY_GAUGE_DEFAULT_MAX_SILENCE_MS - 1u);
    HAC_CHECK_EQ_INT(battery_gauge_state(&g), BATTERY_GAUGE_NO_SAMPLE);
    battery_gauge_sample(&g, false, 0u, start + BATTERY_GAUGE_DEFAULT_MAX_SILENCE_MS);
    HAC_CHECK_EQ_INT(battery_gauge_state(&g), BATTERY_GAUGE_SILENT);
}

static void a_burst_is_due_at_its_interval(void)
{
    battery_gauge_t g = fresh();
    const uint64_t t = 100000u;
    HAC_CHECK(battery_gauge_due(&g, t));
    battery_gauge_sample(&g, true, 3800u, t);
    HAC_CHECK(!battery_gauge_due(&g, t));
    HAC_CHECK(!battery_gauge_due(&g, t + INTERVAL - 1u));
    HAC_CHECK(battery_gauge_due(&g, t + INTERVAL));

    // A refused burst waits its turn too: the ADC is asked no more often.
    battery_gauge_sample(&g, false, 0u, t + INTERVAL);
    HAC_CHECK(!battery_gauge_due(&g, t + INTERVAL + 1u));
    HAC_CHECK(battery_gauge_due(&g, t + 2u * INTERVAL));

    // The monotonic clock restarted: the old stamp means nothing.
    HAC_CHECK(battery_gauge_due(&g, 0u));
}

// A cell going from full to empty over a run, with a sag in two bursts of
// every five -- the panel, the radio. The number only falls, by the steps, and
// says 100 at the start and 0 at the end.
static void a_whole_discharge_only_falls(void)
{
    battery_gauge_t g = fresh();
    uint64_t t = 0u;
    uint32_t mv = 4200u;
    float last = 100.0f;
    unsigned changes = 0u;
    for (unsigned burst = 0u; mv > 3280u; ++burst) {
        const bool sag = (burst % 5u) < 2u;
        battery_gauge_sample(&g, true, sag ? mv - 120u : mv, t);
        t += INTERVAL;
        if ((burst % 5u) == 4u) {
            mv -= 2u;  // a slope far gentler than the window
        }
        if (burst >= 4u) {
            const float now = shown(&g, t);
            hac_test_check(now <= last, "the number only falls", __FILE__, __LINE__);
            if (now != last) {
                changes++;
            }
            last = now;
        }
    }
    HAC_CHECK_EQ_F32(last, 0.0f, 0.0f);
    HAC_CHECK(changes <= 100u / BATTERY_GAUGE_DEFAULT_PERCENT_STEP + 1u);
}

static void every_state_has_a_name(void)
{
    HAC_CHECK(strcmp(battery_gauge_state_name(BATTERY_GAUGE_NO_SAMPLE), "no-sample") == 0);
    HAC_CHECK(strcmp(battery_gauge_state_name(BATTERY_GAUGE_CELL), "cell") == 0);
    HAC_CHECK(strcmp(battery_gauge_state_name(BATTERY_GAUGE_NOT_A_CELL), "not-a-cell") == 0);
    HAC_CHECK(strcmp(battery_gauge_state_name(BATTERY_GAUGE_SILENT), "silent") == 0);
}

int main(void)
{
    HAC_RUN(the_table_holds_at_its_points_and_past_its_ends);
    HAC_RUN(between_two_points_it_is_linear_and_rounds_to_nearest);
    HAC_RUN(nothing_is_known_before_the_first_burst);
    HAC_RUN(the_first_burst_is_the_first_number);
    HAC_RUN(two_sags_of_five_do_not_reach_the_number);
    HAC_RUN(the_number_moves_in_steps);
    HAC_RUN(either_end_is_reached_whatever_the_step);
    HAC_RUN(above_a_cell_the_voltage_is_the_supplys);
    HAC_RUN(the_limits_are_exact_for_the_first_burst);
    HAC_RUN(coming_back_inside_takes_the_hysteresis);
    HAC_RUN(a_new_cell_takes_its_number_at_once);
    HAC_RUN(refused_bursts_keep_the_number_until_the_silence);
    HAC_RUN(an_adc_that_never_answers_is_an_error_in_time);
    HAC_RUN(a_burst_is_due_at_its_interval);
    HAC_RUN(a_whole_discharge_only_falls);
    HAC_RUN(every_state_has_a_name);
    return hac_test_summary();
}
