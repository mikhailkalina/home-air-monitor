// SPDX-License-Identifier: Apache-2.0
//
// update_policy decides when the panel is redrawn, which on real hardware is
// the difference between a readable screen and a damaged one. Every case here
// runs on the fake clock, so "a day passes" and "the partial budget runs out
// over twenty redraws" cost microseconds.

#include <string.h>

#include "app/update_policy.h"
#include "fake_clock.h"
#include "hac_test.h"

// Deliberately not the ED047TC1's numbers: the point of the module is that it
// holds none of its own, so the test states its own panel.
#define TEST_MIN_FULL_INTERVAL_MS 60000u
#define TEST_MAX_PARTIALS 5u

typedef struct {
    fake_clock_t clock;
    update_policy_t policy;
} fixture_t;

static void fixture_init(fixture_t *f, uint32_t min_full_interval_ms, uint32_t max_partials)
{
    fake_clock_init(&f->clock);

    // Every redraw at once: the one-per-cycle limit has its own cases below,
    // and the others step the clock by less than a cycle.
    update_policy_config_t cfg = update_policy_config_default();
    cfg.min_redraw_interval_ms = 0u;
    const update_policy_limits_t limits = {
        .min_full_refresh_interval_ms = min_full_interval_ms,
        .max_partial_refreshes_before_full = max_partials,
    };
    update_policy_init(&f->policy, fake_clock_port(&f->clock), &cfg, &limits);
}

static air_reading_t reading_at(fixture_t *f, float co2, float temperature, float humidity)
{
    const uint64_t now = f->clock.now_ms;
    air_reading_t r;

    air_reading_init(&r);
    r.co2_ppm = measurement_ok(co2, now, 1u);
    r.temperature_c = measurement_ok(temperature, now, 2u);
    r.humidity_rh = measurement_ok(humidity, now, 2u);
    return r;
}

// Draws the first frame so that later cases start from a settled baseline.
static air_reading_t prime(fixture_t *f, float co2, float temperature, float humidity)
{
    air_reading_t r = reading_at(f, co2, temperature, humidity);
    update_decision_t d;

    update_policy_evaluate(&f->policy, &r, &d);
    update_policy_commit(&f->policy, &r, &d);
    return r;
}

static void first_frame_is_a_full_refresh(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);

    const air_reading_t r = reading_at(&f, 620.0f, 22.0f, 45.0f);
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);

    // There is no previous image for a partial refresh to build on.
    HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_FIRST_FRAME);
    HAC_CHECK(!d.full_forced_by_budget);
    HAC_CHECK(!d.deferred_by_min_interval);
}

static void a_crossed_threshold_triggers_a_partial_refresh(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    (void)prime(&f, 620.0f, 22.0f, 45.0f);

    fake_clock_advance(&f.clock, 5000u);

    // 620 -> 680 is 60 ppm, past the 50 ppm threshold of requirements 9.1.
    const air_reading_t r = reading_at(&f, 680.0f, 22.0f, 45.0f);
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);

    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_CO2);
    HAC_CHECK_EQ_U64(d.next_deadline_ms, f.clock.now_ms);
}

static void a_threshold_not_crossed_leaves_the_panel_alone(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    (void)prime(&f, 620.0f, 22.0f, 45.0f);

    fake_clock_advance(&f.clock, 5000u);

    // Every quantity moves, none of them by enough: 40 ppm, 0.1 C, 0.5 %.
    const air_reading_t r = reading_at(&f, 660.0f, 22.1f, 45.5f);
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);

    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_NONE);
    // Nothing to do until something moves, and no timer either.
    HAC_CHECK_EQ_U64(d.next_deadline_ms, UPDATE_POLICY_NO_DEADLINE);
}

static void the_temperature_and_humidity_thresholds_apply_too(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    (void)prime(&f, 620.0f, 22.0f, 45.0f);
    fake_clock_advance(&f.clock, 5000u);

    update_decision_t d;

    const air_reading_t warmer = reading_at(&f, 620.0f, 22.3f, 45.0f);
    update_policy_evaluate(&f.policy, &warmer, &d);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_TEMPERATURE);

    const air_reading_t damper = reading_at(&f, 620.0f, 22.0f, 46.5f);
    update_policy_evaluate(&f.policy, &damper, &d);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_HUMIDITY);
}

// Pressure has a threshold of its own (docs/adr/0027-pressure-redraw-delta.md):
// before it, the weather moved it by 5-9 hPa a day and the screen followed
// only when something else redrew. Strictly more than the delta, either way.
static void the_pressure_threshold_applies_too(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    air_reading_t r = reading_at(&f, 620.0f, 22.0f, 45.0f);
    r.pressure_hpa = measurement_ok(1006.0f, f.clock.now_ms, 2u);
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);
    update_policy_commit(&f.policy, &r, &d);
    fake_clock_advance(&f.clock, 5000u);

    const float delta = UPDATE_POLICY_DEFAULT_PRESSURE_DELTA_HPA;
    const float pressures[] = {1006.0f + delta, 1006.0f - delta, 1006.0f + delta + 0.01f,
                               1006.0f - delta - 0.01f};
    const update_reason_t want[] = {UPDATE_REASON_NONE, UPDATE_REASON_NONE, UPDATE_REASON_PRESSURE,
                                    UPDATE_REASON_PRESSURE};
    for (size_t i = 0u; i < 4u; ++i) {
        r = reading_at(&f, 620.0f, 22.0f, 45.0f);
        r.pressure_hpa = measurement_ok(pressures[i], f.clock.now_ms, 2u);
        update_policy_evaluate(&f.policy, &r, &d);
        HAC_CHECK_EQ_INT(d.reason, want[i]);
    }
    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    HAC_CHECK(strcmp(update_policy_reason_name(d.reason), "pressure") == 0);
}

// The same reading with every quantity aged out: what the screen holds when
// the sensors have stopped delivering.
static air_reading_t stale(air_reading_t r)
{
    r.co2_ppm.status = VAL_STALE;
    r.temperature_c.status = VAL_STALE;
    r.humidity_rh.status = VAL_STALE;
    return r;
}

// Data going stale reaches the glass by itself: VAL_STALE is a status, and
// its arrival is a status change -- the stale ink and the clock mark are
// drawn. Nothing redraws after that while it stays stale: the footer says
// when the values were measured, which stays true
// (docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md).
static void a_reading_going_stale_redraws_once(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    const air_reading_t fresh = prime(&f, 620.0f, 22.0f, 45.0f);

    fake_clock_advance(&f.clock, 31000u);
    const air_reading_t r = stale(fresh);
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_STATUS);
    update_policy_commit(&f.policy, &r, &d);

    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
    HAC_CHECK_EQ_U64(d.next_deadline_ms, UPDATE_POLICY_NO_DEADLINE);
    fake_clock_advance(&f.clock, 24u * 3600u * 1000u);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
    HAC_CHECK_EQ_U64(d.next_deadline_ms, UPDATE_POLICY_NO_DEADLINE);
}

// Fresh data that does not move: a redraw would draw the same picture, and
// there is no timer at all.
static void a_fresh_unchanged_reading_is_never_redrawn_on_a_timer(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    const air_reading_t r = prime(&f, 620.0f, 22.0f, 45.0f);

    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_U64(d.next_deadline_ms, UPDATE_POLICY_NO_DEADLINE);

    fake_clock_advance(&f.clock, 24u * 3600u * 1000u);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
    HAC_CHECK_EQ_U64(d.next_deadline_ms, UPDATE_POLICY_NO_DEADLINE);
}

// Nothing carries a number -- every sensor failed: no timer either.
static void with_no_number_on_the_screen_there_is_no_timer(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    air_reading_t r;
    air_reading_init(&r);
    r.co2_ppm = measurement_status(VAL_ERROR, 0u, 1u);
    r.temperature_c = measurement_status(VAL_ERROR, 0u, 2u);
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);
    update_policy_commit(&f.policy, &r, &d);

    fake_clock_advance(&f.clock, 24u * 3600u * 1000u);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
    HAC_CHECK_EQ_U64(d.next_deadline_ms, UPDATE_POLICY_NO_DEADLINE);
}

// The wall time becoming known turns the footer's "--:--" into a time, and
// that is a status change; the time itself moving on is not, or every poll
// would redraw.
static void the_wall_time_becoming_known_redraws_and_its_ticking_does_not(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    air_reading_t r = prime(&f, 620.0f, 22.0f, 45.0f);  // wall_time_ms 0: "--:--"

    fake_clock_advance(&f.clock, 5000u);
    r.wall_time_ms = 1792890000000u;  // set at the console
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_STATUS);
    update_policy_commit(&f.policy, &r, &d);

    fake_clock_advance(&f.clock, 3600u * 1000u);
    r.wall_time_ms += 3600u * 1000u;
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
    HAC_CHECK_EQ_U64(d.next_deadline_ms, UPDATE_POLICY_NO_DEADLINE);
}

static void a_status_change_alone_reaches_the_glass(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    (void)prime(&f, 620.0f, 22.0f, 45.0f);
    fake_clock_advance(&f.clock, 5000u);

    // Requirements 15.1: the SCD41 failing must be visible even though no
    // number on the screen moved.
    air_reading_t faulted = reading_at(&f, 620.0f, 22.0f, 45.0f);
    faulted.co2_ppm = measurement_status(VAL_ERROR, f.clock.now_ms, 1u);

    update_decision_t d;
    update_policy_evaluate(&f.policy, &faulted, &d);

    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_STATUS);
}

// --- IAQ: its delta, and the accuracy mark ------------------------------------

static air_reading_t with_iaq(fixture_t *f, float iaq, value_accuracy_t accuracy)
{
    air_reading_t r = reading_at(f, 620.0f, 22.0f, 45.0f);
    r.iaq = measurement_ok(iaq, f->clock.now_ms, 2u);
    r.iaq.accuracy = accuracy;
    return r;
}

// Draws `r` as the frame on the glass.
static void draw(fixture_t *f, const air_reading_t *r)
{
    update_decision_t d;
    update_policy_evaluate(&f->policy, r, &d);
    update_policy_commit(&f->policy, r, &d);
}

// The decision's reason for `iaq`, the frame on the glass staying as it is.
static update_reason_t iaq_reason(fixture_t *f, float iaq)
{
    const air_reading_t r = with_iaq(f, iaq, ACC_HIGH);
    update_decision_t d;
    update_policy_evaluate(&f->policy, &r, &d);
    return d.reason;
}

// IAQ redraws as CO2, temperature and humidity do: when its number has moved
// by more than its delta, in its band or out of it
// (docs/adr/0026-iaq-redraw-delta.md).
static void an_iaq_moving_past_its_delta_redraws_whatever_its_band(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    air_reading_t r = with_iaq(&f, 51.0f, ACC_HIGH);
    draw(&f, &r);
    fake_clock_advance(&f.clock, 5000u);

    // "Good" both.
    const float within = 51.0f + UPDATE_POLICY_DEFAULT_IAQ_DELTA + 0.5f;
    update_decision_t d;
    r = with_iaq(&f, within, ACC_HIGH);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_IAQ);
    HAC_CHECK(strcmp(update_policy_reason_name(d.reason), "iaq") == 0);
    update_policy_commit(&f.policy, &r, &d);
    fake_clock_advance(&f.clock, 5000u);

    // "Good" to "Fair".
    r = with_iaq(&f, 100.0f, ACC_HIGH);
    draw(&f, &r);
    fake_clock_advance(&f.clock, 5000u);
    HAC_CHECK_EQ_INT(iaq_reason(&f, 100.0f + UPDATE_POLICY_DEFAULT_IAQ_DELTA + 0.5f),
                     UPDATE_REASON_IAQ);
}

// A change of band is not a reason of its own: 50 to 51 is "Excellent" to
// "Good", and one point.
static void a_change_of_band_alone_does_not_redraw(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    air_reading_t r = with_iaq(&f, 50.0f, ACC_HIGH);
    draw(&f, &r);
    fake_clock_advance(&f.clock, 5000u);
    HAC_CHECK_EQ_INT(iaq_reason(&f, 51.0f), UPDATE_REASON_NONE);

    r = with_iaq(&f, 100.0f, ACC_HIGH);
    draw(&f, &r);
    fake_clock_advance(&f.clock, 5000u);
    HAC_CHECK_EQ_INT(iaq_reason(&f, 101.0f), UPDATE_REASON_NONE);
}

// BSEC's clean air is 50, the Excellent/Good boundary, and on the board the
// index trembles across it all day: most of its excursions stay within 5 of
// it, and dip to 48 below. None of that redraws.
static void an_iaq_trembling_on_a_boundary_never_redraws(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    air_reading_t r = with_iaq(&f, 50.0f, ACC_HIGH);
    draw(&f, &r);

    static const float noise[] = {51.0f, 50.0f, 51.4f, 49.6f, 52.3f, 50.0f, 55.0f,
                                  50.4f, 50.5f, 48.1f, 54.8f, 50.0f, 53.7f, 50.2f};
    for (unsigned loop = 0u; loop < 10u; ++loop) {
        for (size_t i = 0u; i < sizeof(noise) / sizeof(noise[0]); ++i) {
            fake_clock_advance(&f.clock, 3000u);  // BSEC's sample period
            HAC_CHECK_EQ_INT(iaq_reason(&f, noise[i]), UPDATE_REASON_NONE);
        }
    }
}

// Strictly more, as every delta here.
static void exactly_the_iaq_delta_does_not_redraw(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    air_reading_t r = with_iaq(&f, 50.0f, ACC_HIGH);
    draw(&f, &r);
    fake_clock_advance(&f.clock, 5000u);

    HAC_CHECK_EQ_INT(iaq_reason(&f, 50.0f + UPDATE_POLICY_DEFAULT_IAQ_DELTA), UPDATE_REASON_NONE);
    HAC_CHECK_EQ_INT(iaq_reason(&f, 50.0f + UPDATE_POLICY_DEFAULT_IAQ_DELTA + 0.01f),
                     UPDATE_REASON_IAQ);
    HAC_CHECK_EQ_INT(iaq_reason(&f, 50.0f - UPDATE_POLICY_DEFAULT_IAQ_DELTA), UPDATE_REASON_NONE);
    HAC_CHECK_EQ_INT(iaq_reason(&f, 50.0f - UPDATE_POLICY_DEFAULT_IAQ_DELTA - 0.01f),
                     UPDATE_REASON_IAQ);
}

// A slow rise redraws each time it has left the last frame behind by more
// than the delta, and not in between.
static void a_slow_drift_redraws_each_time_it_passes_the_delta(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    air_reading_t r = with_iaq(&f, 50.0f, ACC_HIGH);
    draw(&f, &r);

    float drawn = 50.0f;
    unsigned redraws = 0u;
    for (float iaq = 51.0f; iaq <= 80.0f; iaq += 1.0f) {
        fake_clock_advance(&f.clock, 5000u);
        r = with_iaq(&f, iaq, ACC_HIGH);
        update_decision_t d;
        update_policy_evaluate(&f.policy, &r, &d);
        const bool due = iaq - drawn > UPDATE_POLICY_DEFAULT_IAQ_DELTA;
        HAC_CHECK_EQ_INT(d.reason, due ? UPDATE_REASON_IAQ : UPDATE_REASON_NONE);
        if (d.action != UPDATE_NONE) {
            update_policy_commit(&f.policy, &r, &d);
            drawn = iaq;
            redraws++;
        }
    }
    HAC_CHECK_EQ_INT(redraws, 5);  // at 56, 62, 68, 74 and 80 with a delta of 5
}

// The rarer excursions that pass the delta are what it costs: a redraw up,
// and one back once the index is down by more than the delta again.
static void an_excursion_past_the_delta_redraws_up_and_back(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    air_reading_t r = with_iaq(&f, 50.0f, ACC_HIGH);
    draw(&f, &r);
    fake_clock_advance(&f.clock, 3000u);

    const float up = 50.0f + UPDATE_POLICY_DEFAULT_IAQ_DELTA + 1.6f;  // 56.6 with 5
    HAC_CHECK_EQ_INT(iaq_reason(&f, up), UPDATE_REASON_IAQ);
    r = with_iaq(&f, up, ACC_HIGH);
    draw(&f, &r);
    fake_clock_advance(&f.clock, 3000u);

    HAC_CHECK_EQ_INT(iaq_reason(&f, up - 1.0f), UPDATE_REASON_NONE);
    HAC_CHECK_EQ_INT(iaq_reason(&f, 50.2f), UPDATE_REASON_IAQ);
}

// Within the delta the glass keeps its frame, band and all: "54 Good" while
// the air reads "50 Excellent" -- a number and its band that agree, 4 points
// behind the air, as any field may be within its delta.
static void within_the_delta_the_frame_keeps_its_band(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    air_reading_t r = with_iaq(&f, 54.0f, ACC_HIGH);
    draw(&f, &r);
    fake_clock_advance(&f.clock, 5000u);
    HAC_CHECK_EQ_INT(iaq_reason(&f, 50.0f), UPDATE_REASON_NONE);
}

// A redraw for anything else draws the IAQ as it is then, and that frame is
// what the next move is measured from.
static void another_fields_redraw_is_the_new_iaq_baseline(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    air_reading_t r = with_iaq(&f, 50.0f, ACC_HIGH);
    draw(&f, &r);
    fake_clock_advance(&f.clock, 5000u);

    HAC_CHECK_EQ_INT(iaq_reason(&f, 55.0f), UPDATE_REASON_NONE);

    // CO2 moves past its delta: the frame now shows "55 Good".
    r = with_iaq(&f, 55.0f, ACC_HIGH);
    r.co2_ppm.value = 700.0f;
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_CO2);
    update_policy_commit(&f.policy, &r, &d);
    fake_clock_advance(&f.clock, 5000u);

    // Measured from 55, not from the first frame's 50. with_iaq()'s CO2 is
    // back at 620 -- keep the frame's 700 so only IAQ is judged.
    const float iaqs[] = {50.0f, 60.0f, 49.9f};
    const update_reason_t want[] = {UPDATE_REASON_NONE, UPDATE_REASON_NONE, UPDATE_REASON_IAQ};
    for (size_t i = 0u; i < 3u; ++i) {
        r = with_iaq(&f, iaqs[i], ACC_HIGH);
        r.co2_ppm.value = 700.0f;
        update_policy_evaluate(&f.policy, &r, &d);
        HAC_CHECK_EQ_INT(d.reason, want[i]);
    }
}

// The delta holds back a number only: a number lost, or the question mark
// going, reaches the glass at once, however little the index moved.
static void status_and_accuracy_changes_ignore_the_iaq_delta(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    air_reading_t r = with_iaq(&f, 50.0f, ACC_LOW);
    draw(&f, &r);
    fake_clock_advance(&f.clock, 5000u);

    update_decision_t d;
    r = with_iaq(&f, 51.0f, ACC_MEDIUM);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_STATUS);
    update_policy_commit(&f.policy, &r, &d);
    fake_clock_advance(&f.clock, 5000u);

    r = with_iaq(&f, 50.0f, ACC_MEDIUM);
    r.iaq = measurement_status(VAL_ERROR, f.clock.now_ms, 2u);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_STATUS);
}

// The first frame and a person's request draw the trembling index as it is;
// a delta of 0 redraws on any change, as for the other fields.
static void the_first_frame_requests_and_a_zero_delta(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    HAC_CHECK_EQ_INT(iaq_reason(&f, 51.0f), UPDATE_REASON_FIRST_FRAME);
    air_reading_t r = with_iaq(&f, 51.0f, ACC_HIGH);
    draw(&f, &r);
    fake_clock_advance(&f.clock, 5000u);

    update_policy_request(&f.policy);
    HAC_CHECK_EQ_INT(iaq_reason(&f, 50.0f), UPDATE_REASON_REQUEST);
    r = with_iaq(&f, 50.0f, ACC_HIGH);
    draw(&f, &r);
    fake_clock_advance(&f.clock, 5000u);
    HAC_CHECK_EQ_INT(iaq_reason(&f, 51.0f), UPDATE_REASON_NONE);

    f.policy.cfg.iaq_delta = 0.0f;
    HAC_CHECK_EQ_INT(iaq_reason(&f, 50.4f), UPDATE_REASON_IAQ);
    HAC_CHECK_EQ_INT(iaq_reason(&f, 50.0f), UPDATE_REASON_NONE);
}

static void an_iaq_gaining_its_number_is_a_status_change(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    air_reading_t r = reading_at(&f, 620.0f, 22.0f, 45.0f);
    r.iaq = measurement_status(VAL_WARMUP, f.clock.now_ms, 2u);
    r.iaq.accuracy = ACC_UNRELIABLE;
    draw(&f, &r);
    fake_clock_advance(&f.clock, 5000u);

    r = with_iaq(&f, 50.0f, ACC_LOW);
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_STATUS);
}

// Low to medium takes the question mark off the screen; medium to high, and
// back, draws the same -- BSEC goes back and forth between them every few
// minutes on the host, and none of that may cost a partial refresh.
static void only_an_accuracy_change_the_screen_shows_redraws(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    air_reading_t r = with_iaq(&f, 60.0f, ACC_LOW);
    draw(&f, &r);
    fake_clock_advance(&f.clock, 5000u);

    update_decision_t d;
    r = with_iaq(&f, 60.0f, ACC_MEDIUM);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_STATUS);
    update_policy_commit(&f.policy, &r, &d);
    fake_clock_advance(&f.clock, 5000u);

    r = with_iaq(&f, 60.0f, ACC_HIGH);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
    r = with_iaq(&f, 60.0f, ACC_MEDIUM);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);

    // A source that gives no verdict and one that rates itself high draw the
    // same too.
    r = with_iaq(&f, 60.0f, ACC_NOT_REPORTED);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
}

static void the_partial_budget_forces_a_full_refresh(void)
{
    fixture_t f;
    fixture_init(&f, /*min_full_interval_ms=*/0u, TEST_MAX_PARTIALS);
    (void)prime(&f, 620.0f, 22.0f, 45.0f);

    // Spend the budget: TEST_MAX_PARTIALS redraws, each triggered by CO2.
    float co2 = 620.0f;
    for (uint32_t i = 0u; i < TEST_MAX_PARTIALS; ++i) {
        fake_clock_advance(&f.clock, 5000u);
        co2 += 100.0f;

        const air_reading_t r = reading_at(&f, co2, 22.0f, 45.0f);
        update_decision_t d;
        update_policy_evaluate(&f.policy, &r, &d);
        HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
        update_policy_commit(&f.policy, &r, &d);
    }
    HAC_CHECK_EQ_INT(update_policy_partials_since_full(&f.policy), (int)TEST_MAX_PARTIALS);

    // The next redraw would be the sixth partial in a row, which is what the
    // vendor warns damages the panel. It comes out full instead.
    fake_clock_advance(&f.clock, 5000u);
    co2 += 100.0f;
    const air_reading_t r = reading_at(&f, co2, 22.0f, 45.0f);
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);

    HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_CO2);
    HAC_CHECK(d.full_forced_by_budget);

    update_policy_commit(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(update_policy_partials_since_full(&f.policy), 0);
}

// docs/adr/0019-partials-past-the-budget.md: the budget is spent and a full
// refresh is due, but the panel's minimum interval has not elapsed. The
// screen is not frozen: the redraw goes out partially, past the budget. Once
// the interval has elapsed nothing is redrawn for that alone; the next redraw
// that is due anyway comes out full.
static void past_the_budget_it_refreshes_partially_until_a_full_refresh_is_allowed(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);

    // The first frame is a full refresh, and starts the minimum-interval
    // clock at t = 0.
    (void)prime(&f, 620.0f, 22.0f, 45.0f);

    float co2 = 620.0f;
    for (uint32_t i = 0u; i < TEST_MAX_PARTIALS; ++i) {
        fake_clock_advance(&f.clock, 1000u);
        co2 += 100.0f;
        const air_reading_t r = reading_at(&f, co2, 22.0f, 45.0f);
        update_decision_t d;
        update_policy_evaluate(&f.policy, &r, &d);
        HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
        HAC_CHECK(!d.partial_past_budget);
        update_policy_commit(&f.policy, &r, &d);
    }

    // The budget is spent, so this redraw would be full -- but only six
    // seconds have passed since the last full refresh.
    fake_clock_advance(&f.clock, 1000u);
    co2 += 100.0f;
    air_reading_t r = reading_at(&f, co2, 22.0f, 45.0f);
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);

    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    HAC_CHECK(d.partial_past_budget);
    HAC_CHECK(!d.full_forced_by_budget);
    HAC_CHECK(!d.deferred_by_min_interval);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_CO2);
    update_policy_commit(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(update_policy_partials_since_full(&f.policy), (int)TEST_MAX_PARTIALS + 1);

    // The interval elapses with nothing new to show: nothing is redrawn for
    // that alone, and with fresh data there is no timer to wait for.
    fake_clock_advance(&f.clock, TEST_MIN_FULL_INTERVAL_MS);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
    HAC_CHECK_EQ_U64(d.next_deadline_ms, UPDATE_POLICY_NO_DEADLINE);

    // The next change comes out full.
    co2 += 100.0f;
    r = reading_at(&f, co2, 22.0f, 45.0f);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_CO2);
    HAC_CHECK(d.full_forced_by_budget);
    update_policy_commit(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(update_policy_partials_since_full(&f.policy), 0);
}

// Past the budget, the data going stale once the interval has elapsed is the
// redraw that comes out full: a status change like any other.
static void past_the_budget_data_going_stale_is_the_full_one(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    (void)prime(&f, 620.0f, 22.0f, 45.0f);

    float co2 = 620.0f;
    air_reading_t r = reading_at(&f, co2, 22.0f, 45.0f);
    update_decision_t d;
    for (uint32_t i = 0u; i <= TEST_MAX_PARTIALS; ++i) {  // one past the budget
        fake_clock_advance(&f.clock, 1000u);
        co2 += 100.0f;
        r = reading_at(&f, co2, 22.0f, 45.0f);
        update_policy_evaluate(&f.policy, &r, &d);
        update_policy_commit(&f.policy, &r, &d);
    }

    fake_clock_advance(&f.clock, TEST_MIN_FULL_INTERVAL_MS);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);

    r = stale(r);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_STATUS);
    HAC_CHECK(d.full_forced_by_budget);
}

// Past the budget with fresh data that stops moving: nothing is redrawn until
// something changes, however long that takes -- and then fully.
static void past_the_budget_with_fresh_data_the_next_change_is_the_full_one(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    (void)prime(&f, 620.0f, 22.0f, 45.0f);

    float co2 = 620.0f;
    air_reading_t r = reading_at(&f, co2, 22.0f, 45.0f);
    update_decision_t d;
    for (uint32_t i = 0u; i <= TEST_MAX_PARTIALS; ++i) {  // one past the budget
        fake_clock_advance(&f.clock, 1000u);
        co2 += 100.0f;
        r = reading_at(&f, co2, 22.0f, 45.0f);
        update_policy_evaluate(&f.policy, &r, &d);
        update_policy_commit(&f.policy, &r, &d);
    }

    fake_clock_advance(&f.clock, 24u * 3600u * 1000u);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);

    r = reading_at(&f, co2 + 100.0f, 22.0f, 45.0f);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_CO2);
}

// At the firmware's pace the budget is never passed. The two sensors are read
// at different moments of each 5 s cycle (phase 3c's log: CO2 at x.08 s, the
// BME68x at x.70 s), so readings move up to every 2.5 s -- but one redraw per
// cycle (UPDATE_POLICY_DEFAULT_MIN_REDRAW_INTERVAL_MS) takes them both, every
// 5 s: the board's budget of 20 is spent at 100 s, long after the 60 s
// interval, and the 21st redraw, at 105 s, is full. Twenty partial refreshes
// between full ones, 105 s apart, at the very most.
static void at_the_firmwares_pace_the_budget_is_never_passed(void)
{
    const uint32_t budget = 20u;
    const uint32_t interval_ms = 60000u;
    const uint32_t step_ms = 2500u;

    fixture_t f;
    fixture_init(&f, interval_ms, budget);
    f.policy.cfg.min_redraw_interval_ms = UPDATE_POLICY_DEFAULT_MIN_REDRAW_INTERVAL_MS;
    (void)prime(&f, 620.0f, 22.0f, 45.0f);

    uint32_t most_partials = 0u;
    uint32_t past_budget = 0u;
    uint64_t last_full_ms = 0u;
    uint64_t shortest_gap_ms = UINT64_MAX;
    float co2 = 620.0f;
    for (uint32_t step = 0u; step < 1440u; ++step) {  // an hour
        fake_clock_advance(&f.clock, step_ms);
        co2 = (co2 > 1500.0f) ? 620.0f : co2 + 60.0f;  // past the 50 ppm threshold every time
        const air_reading_t r = reading_at(&f, co2, 22.0f, 45.0f);
        update_decision_t d;
        update_policy_evaluate(&f.policy, &r, &d);
        if (d.action == UPDATE_NONE) {
            continue;
        }
        HAC_CHECK(!d.deferred_by_min_interval);
        past_budget += d.partial_past_budget ? 1u : 0u;
        update_policy_commit(&f.policy, &r, &d);
        const uint32_t partials = update_policy_partials_since_full(&f.policy);
        most_partials = (partials > most_partials) ? partials : most_partials;
        if (d.action == UPDATE_FULL) {
            const uint64_t gap = f.clock.now_ms - last_full_ms;
            shortest_gap_ms = (gap < shortest_gap_ms) ? gap : shortest_gap_ms;
            last_full_ms = f.clock.now_ms;
        }
    }
    HAC_CHECK_EQ_INT((int)most_partials, (int)budget);
    HAC_CHECK_EQ_INT((int)past_budget, 0);
    HAC_CHECK_EQ_U64(shortest_gap_ms, 105000u);
}

// Without the one-per-cycle limit, and readings moving every 2.5 s, the
// budget is spent at 50 s; partial refreshes go past it at 52.5, 55 and
// 57.5 s, and the redraw at 60 s is full: 23 between full ones. What bounds
// past-the-budget refreshes when redraws come faster than the sensors' cycle.
static void past_the_budget_is_bounded_by_the_interval(void)
{
    const uint32_t budget = 20u;
    const uint32_t interval_ms = 60000u;
    const uint32_t step_ms = 2500u;

    fixture_t f;
    fixture_init(&f, interval_ms, budget);
    (void)prime(&f, 620.0f, 22.0f, 45.0f);

    uint32_t most_partials = 0u;
    uint64_t last_full_ms = 0u;
    uint64_t longest_gap_ms = 0u;
    float co2 = 620.0f;
    for (uint32_t step = 0u; step < 1440u; ++step) {  // an hour
        fake_clock_advance(&f.clock, step_ms);
        co2 = (co2 > 1500.0f) ? 620.0f : co2 + 60.0f;
        const air_reading_t r = reading_at(&f, co2, 22.0f, 45.0f);
        update_decision_t d;
        update_policy_evaluate(&f.policy, &r, &d);
        if (d.action == UPDATE_NONE) {
            continue;
        }
        update_policy_commit(&f.policy, &r, &d);
        const uint32_t partials = update_policy_partials_since_full(&f.policy);
        most_partials = (partials > most_partials) ? partials : most_partials;
        if (d.action == UPDATE_FULL) {
            const uint64_t gap = f.clock.now_ms - last_full_ms;
            longest_gap_ms = (gap > longest_gap_ms) ? gap : longest_gap_ms;
            last_full_ms = f.clock.now_ms;
        }
    }
    HAC_CHECK_EQ_INT((int)most_partials, 23);
    HAC_CHECK(longest_gap_ms <= interval_ms);
}

// --- one redraw per cycle -------------------------------------------------

// The CO2 reading moves and is drawn; 0.6 s later, in the same cycle, the
// temperature moves too. It waits for the cycle to end, and goes out then.
static void a_second_redraw_within_one_cycle_waits_for_the_cycle(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    f.policy.cfg.min_redraw_interval_ms = UPDATE_POLICY_DEFAULT_MIN_REDRAW_INTERVAL_MS;
    (void)prime(&f, 620.0f, 22.0f, 45.0f);

    fake_clock_advance(&f.clock, 5000u);
    air_reading_t r = reading_at(&f, 700.0f, 22.0f, 45.0f);
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    update_policy_commit(&f.policy, &r, &d);
    const uint64_t drawn_at = f.clock.now_ms;

    fake_clock_advance(&f.clock, 600u);
    r = reading_at(&f, 700.0f, 22.5f, 45.0f);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
    HAC_CHECK(d.held_for_redraw_interval);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_TEMPERATURE);  // what was wanted, and why it waits
    HAC_CHECK_EQ_U64(d.next_deadline_ms, drawn_at + UPDATE_POLICY_DEFAULT_MIN_REDRAW_INTERVAL_MS);

    fake_clock_advance(&f.clock, d.next_deadline_ms - f.clock.now_ms);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    HAC_CHECK(!d.held_for_redraw_interval);
}

// A sensor error waits for the cycle too -- at most 5 s, well inside
// requirements 15.1's "shown on screen".
static void a_status_change_waits_no_longer_than_one_cycle(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    f.policy.cfg.min_redraw_interval_ms = UPDATE_POLICY_DEFAULT_MIN_REDRAW_INTERVAL_MS;
    air_reading_t r = prime(&f, 620.0f, 22.0f, 45.0f);

    fake_clock_advance(&f.clock, 1000u);
    r.co2_ppm = measurement_status(VAL_ERROR, f.clock.now_ms, 1u);
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
    HAC_CHECK(d.held_for_redraw_interval);
    HAC_CHECK(d.next_deadline_ms - f.clock.now_ms <= UPDATE_POLICY_DEFAULT_MIN_REDRAW_INTERVAL_MS);

    fake_clock_advance(&f.clock, d.next_deadline_ms - f.clock.now_ms);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_STATUS);
}

// A spent budget alone is no reason to redraw: with exactly the budget used
// and no change to show, nothing happens when a full refresh becomes allowed.
static void a_budget_spent_exactly_waits_for_the_next_change(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    (void)prime(&f, 620.0f, 22.0f, 45.0f);

    float co2 = 620.0f;
    air_reading_t r = reading_at(&f, co2, 22.0f, 45.0f);
    for (uint32_t i = 0u; i < TEST_MAX_PARTIALS; ++i) {
        fake_clock_advance(&f.clock, 1000u);
        co2 += 100.0f;
        r = reading_at(&f, co2, 22.0f, 45.0f);
        update_decision_t d;
        update_policy_evaluate(&f.policy, &r, &d);
        update_policy_commit(&f.policy, &r, &d);
    }

    fake_clock_advance(&f.clock, TEST_MIN_FULL_INTERVAL_MS);
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
}

// The only case left where the screen waits: a panel with no partial mode
// has nothing to keep it current with while a full refresh is not allowed.
static void a_panel_without_a_partial_mode_defers_until_a_full_refresh_is_allowed(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, /*max_partials=*/0u);
    (void)prime(&f, 620.0f, 22.0f, 45.0f);

    fake_clock_advance(&f.clock, 5000u);
    const air_reading_t r = reading_at(&f, 800.0f, 22.0f, 45.0f);
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);

    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
    HAC_CHECK(d.deferred_by_min_interval);
    HAC_CHECK_EQ_U64(d.next_deadline_ms, TEST_MIN_FULL_INTERVAL_MS);

    // Committing a deferred decision must not move the baseline: nothing
    // reached the panel.
    update_policy_commit(&f.policy, &r, &d);
    fake_clock_advance(&f.clock, TEST_MIN_FULL_INTERVAL_MS);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_CO2);
}

static void a_panel_without_a_partial_mode_only_ever_refreshes_fully(void)
{
    fixture_t f;
    fixture_init(&f, /*min_full_interval_ms=*/0u, /*max_partials=*/0u);
    (void)prime(&f, 620.0f, 22.0f, 45.0f);

    fake_clock_advance(&f.clock, 5000u);
    const air_reading_t r = reading_at(&f, 800.0f, 22.0f, 45.0f);
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);

    HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);
    HAC_CHECK(d.full_forced_by_budget);
}

// --- a person's redraw (docs/adr/0025-touch-input-gt911.md) ----------------------

// A fixture with the firmware's one-redraw-per-cycle limit in place.
static void fixture_init_with_cycle(fixture_t *f, uint32_t min_full_interval_ms,
                                    uint32_t max_partials)
{
    fake_clock_init(&f->clock);
    const update_policy_config_t cfg = update_policy_config_default();
    const update_policy_limits_t limits = {
        .min_full_refresh_interval_ms = min_full_interval_ms,
        .max_partial_refreshes_before_full = max_partials,
    };
    update_policy_init(&f->policy, fake_clock_port(&f->clock), &cfg, &limits);
}

static void a_request_goes_out_at_once_without_waiting_for_the_cycle(void)
{
    fixture_t f;
    fixture_init_with_cycle(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    const air_reading_t r0 = prime(&f, 620.0f, 22.0f, 45.0f);
    fake_clock_advance(&f.clock, 1000u);

    // A reading change this soon after the first frame waits for the cycle...
    const air_reading_t moved = reading_at(&f, 700.0f, 22.0f, 45.0f);
    update_decision_t d;
    update_policy_evaluate(&f.policy, &moved, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
    HAC_CHECK(d.held_for_redraw_interval);

    // ...a request does not, and is a partial refresh.
    update_policy_request(&f.policy);
    update_policy_evaluate(&f.policy, &r0, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_REQUEST);
    HAC_CHECK(!d.held_for_redraw_interval);
    HAC_CHECK_EQ_U64(d.next_deadline_ms, f.clock.now_ms);

    // It is counted like any other partial, and answered by the commit.
    update_policy_commit(&f.policy, &r0, &d);
    HAC_CHECK_EQ_INT((int)update_policy_partials_since_full(&f.policy), 1);
    update_policy_evaluate(&f.policy, &r0, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
}

static void a_request_stays_due_until_a_redraw_reaches_the_panel(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    const air_reading_t r = prime(&f, 620.0f, 22.0f, 45.0f);
    update_policy_request(&f.policy);
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_REQUEST);
    // A flush that failed commits nothing: still due.
    fake_clock_advance(&f.clock, 1000u);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_REQUEST);
}

static void a_request_with_the_budget_spent_is_full_when_a_full_one_is_allowed(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    air_reading_t r = prime(&f, 620.0f, 22.0f, 45.0f);
    update_decision_t d;
    for (uint32_t i = 0u; i < TEST_MAX_PARTIALS; ++i) {
        fake_clock_advance(&f.clock, 20000u);
        update_policy_request(&f.policy);
        update_policy_evaluate(&f.policy, &r, &d);
        HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
        update_policy_commit(&f.policy, &r, &d);
    }
    // The budget is spent, and the interval long since elapsed.
    update_policy_request(&f.policy);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_REQUEST);
    HAC_CHECK(d.full_forced_by_budget);
    update_policy_commit(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT((int)update_policy_partials_since_full(&f.policy), 0);
}

static void a_request_past_the_budget_before_the_interval_is_partial(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    air_reading_t r = prime(&f, 620.0f, 22.0f, 45.0f);
    update_decision_t d;
    for (uint32_t i = 0u; i < TEST_MAX_PARTIALS; ++i) {
        fake_clock_advance(&f.clock, 1000u);
        update_policy_request(&f.policy);
        update_policy_evaluate(&f.policy, &r, &d);
        update_policy_commit(&f.policy, &r, &d);
    }
    // Five taps in five seconds: the budget spent long before the panel's
    // minute -- the screen still changes, partially, past the budget.
    fake_clock_advance(&f.clock, 1000u);
    update_policy_request(&f.policy);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    HAC_CHECK(d.partial_past_budget);
    update_policy_commit(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT((int)update_policy_partials_since_full(&f.policy), (int)TEST_MAX_PARTIALS + 1);
}

static void a_request_before_the_first_frame_is_the_first_frame(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    update_policy_request(&f.policy);
    const air_reading_t r = reading_at(&f, 620.0f, 22.0f, 45.0f);
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_FIRST_FRAME);
    update_policy_commit(&f.policy, &r, &d);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);  // the first frame answered it
}

static void a_screen_that_compares_its_own_content_follows_the_same_rules(void)
{
    fixture_t f;
    fixture_init_with_cycle(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    update_decision_t d;
    update_policy_evaluate_content(&f.policy, false, &d);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_FIRST_FRAME);
    HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);
    const air_reading_t r = reading_at(&f, 620.0f, 22.0f, 45.0f);
    update_policy_commit(&f.policy, &r, &d);

    update_policy_evaluate_content(&f.policy, false, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);

    // Changed within the cycle: held, then partial.
    fake_clock_advance(&f.clock, 1000u);
    update_policy_evaluate_content(&f.policy, true, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
    HAC_CHECK(d.held_for_redraw_interval);
    fake_clock_advance(&f.clock, UPDATE_POLICY_DEFAULT_MIN_REDRAW_INTERVAL_MS);
    update_policy_evaluate_content(&f.policy, true, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_STATUS);

    // A request on it goes out at once too.
    update_policy_commit(&f.policy, &r, &d);
    update_policy_request(&f.policy);
    update_policy_evaluate_content(&f.policy, false, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_REQUEST);
    HAC_CHECK(strcmp(update_policy_reason_name(d.reason), "request") == 0);
}

// A person tapping without pause, a screen change every 1.2 s from just
// after a full refresh: past the budget by the budget again, then even a
// request waits for the full refresh, which it then is.
static void past_the_budget_by_the_budget_again_even_a_request_waits(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    const air_reading_t r = prime(&f, 620.0f, 22.0f, 45.0f);
    const uint32_t most = TEST_MAX_PARTIALS * UPDATE_POLICY_MOST_PARTIALS_PER_BUDGET;
    update_decision_t d;
    for (uint32_t i = 0u; i < most; ++i) {
        fake_clock_advance(&f.clock, 1200u);
        update_policy_request(&f.policy);
        update_policy_evaluate(&f.policy, &r, &d);
        HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
        update_policy_commit(&f.policy, &r, &d);
    }
    HAC_CHECK_EQ_INT((int)update_policy_partials_since_full(&f.policy), (int)most);

    fake_clock_advance(&f.clock, 1200u);
    update_policy_request(&f.policy);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
    HAC_CHECK(d.deferred_by_min_interval);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_REQUEST);
    HAC_CHECK_EQ_U64(d.next_deadline_ms, TEST_MIN_FULL_INTERVAL_MS);  // the first frame was at 0

    fake_clock_advance(&f.clock, d.next_deadline_ms - f.clock.now_ms);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_REQUEST);
}

static void limits_come_from_the_display_port(void)
{
    static port_display_t display;  // static: the unread members start zeroed
    display.min_full_refresh_interval_ms = 12345u;
    display.max_partial_refreshes_before_full = 17u;

    const update_policy_limits_t limits = update_policy_limits_from_display(&display);
    HAC_CHECK_EQ_U64(limits.min_full_refresh_interval_ms, 12345u);
    HAC_CHECK_EQ_U64(limits.max_partial_refreshes_before_full, 17u);

    // No panel to ask: refuse partial refreshes rather than guess a budget.
    const update_policy_limits_t none = update_policy_limits_from_display(NULL);
    HAC_CHECK_EQ_U64(none.max_partial_refreshes_before_full, 0u);
}

static void the_defaults_are_the_thresholds_from_the_requirements(void)
{
    const update_policy_config_t cfg = update_policy_config_default();

    HAC_CHECK_EQ_F32(cfg.co2_delta_ppm, 50.0f, 0.001f);
    HAC_CHECK_EQ_F32(cfg.temperature_delta_c, 0.2f, 0.001f);
    HAC_CHECK_EQ_F32(cfg.humidity_delta_rh, 1.0f, 0.001f);
    // ADR 0027, not a requirement.
    HAC_CHECK_EQ_F32(cfg.pressure_delta_hpa, 1.0f, 0.001f);
    // ADR 0026, not a requirement.
    HAC_CHECK_EQ_F32(cfg.iaq_delta, 5.0f, 0.001f);
    HAC_CHECK_EQ_U64(cfg.min_redraw_interval_ms, 5000u);  // one sensor cycle, not a requirement
}

int main(void)
{
    HAC_RUN(first_frame_is_a_full_refresh);
    HAC_RUN(a_crossed_threshold_triggers_a_partial_refresh);
    HAC_RUN(a_threshold_not_crossed_leaves_the_panel_alone);
    HAC_RUN(the_temperature_and_humidity_thresholds_apply_too);
    HAC_RUN(the_pressure_threshold_applies_too);
    HAC_RUN(a_reading_going_stale_redraws_once);
    HAC_RUN(a_fresh_unchanged_reading_is_never_redrawn_on_a_timer);
    HAC_RUN(with_no_number_on_the_screen_there_is_no_timer);
    HAC_RUN(the_wall_time_becoming_known_redraws_and_its_ticking_does_not);
    HAC_RUN(a_status_change_alone_reaches_the_glass);
    HAC_RUN(an_iaq_moving_past_its_delta_redraws_whatever_its_band);
    HAC_RUN(a_change_of_band_alone_does_not_redraw);
    HAC_RUN(an_iaq_trembling_on_a_boundary_never_redraws);
    HAC_RUN(exactly_the_iaq_delta_does_not_redraw);
    HAC_RUN(a_slow_drift_redraws_each_time_it_passes_the_delta);
    HAC_RUN(an_excursion_past_the_delta_redraws_up_and_back);
    HAC_RUN(within_the_delta_the_frame_keeps_its_band);
    HAC_RUN(another_fields_redraw_is_the_new_iaq_baseline);
    HAC_RUN(status_and_accuracy_changes_ignore_the_iaq_delta);
    HAC_RUN(the_first_frame_requests_and_a_zero_delta);
    HAC_RUN(an_iaq_gaining_its_number_is_a_status_change);
    HAC_RUN(only_an_accuracy_change_the_screen_shows_redraws);
    HAC_RUN(the_partial_budget_forces_a_full_refresh);
    HAC_RUN(past_the_budget_it_refreshes_partially_until_a_full_refresh_is_allowed);
    HAC_RUN(a_budget_spent_exactly_waits_for_the_next_change);
    HAC_RUN(past_the_budget_data_going_stale_is_the_full_one);
    HAC_RUN(past_the_budget_with_fresh_data_the_next_change_is_the_full_one);
    HAC_RUN(at_the_firmwares_pace_the_budget_is_never_passed);
    HAC_RUN(a_second_redraw_within_one_cycle_waits_for_the_cycle);
    HAC_RUN(a_status_change_waits_no_longer_than_one_cycle);
    HAC_RUN(past_the_budget_is_bounded_by_the_interval);
    HAC_RUN(a_panel_without_a_partial_mode_defers_until_a_full_refresh_is_allowed);
    HAC_RUN(a_panel_without_a_partial_mode_only_ever_refreshes_fully);
    HAC_RUN(a_request_goes_out_at_once_without_waiting_for_the_cycle);
    HAC_RUN(a_request_stays_due_until_a_redraw_reaches_the_panel);
    HAC_RUN(a_request_with_the_budget_spent_is_full_when_a_full_one_is_allowed);
    HAC_RUN(a_request_past_the_budget_before_the_interval_is_partial);
    HAC_RUN(a_request_before_the_first_frame_is_the_first_frame);
    HAC_RUN(a_screen_that_compares_its_own_content_follows_the_same_rules);
    HAC_RUN(past_the_budget_by_the_budget_again_even_a_request_waits);
    HAC_RUN(limits_come_from_the_display_port);
    HAC_RUN(the_defaults_are_the_thresholds_from_the_requirements);
    return hac_test_summary();
}
