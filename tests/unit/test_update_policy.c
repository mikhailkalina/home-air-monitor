// SPDX-License-Identifier: Apache-2.0
//
// update_policy decides when the panel is redrawn, which on real hardware is
// the difference between a readable screen and a damaged one. Every case here
// runs on the fake clock, so "two minutes elapse" and "the partial budget runs
// out over twenty redraws" cost microseconds.

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
    // Nothing to do until something moves: the data is fresh, so there is no
    // periodic redraw either.
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

// The same reading with every quantity aged out: what the screen holds when
// the sensors have stopped delivering.
static air_reading_t stale(air_reading_t r)
{
    r.co2_ppm.status = VAL_STALE;
    r.temperature_c.status = VAL_STALE;
    r.humidity_rh.status = VAL_STALE;
    return r;
}

// Stale data: the "updated N ago" line grows, and the periodic redraw keeps
// it counting.
static void a_stale_reading_redraws_on_the_periodic_timeout(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    const air_reading_t r = stale(reading_at(&f, 620.0f, 22.0f, 45.0f));
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);
    update_policy_commit(&f.policy, &r, &d);  // the first frame, at t = 0

    // One millisecond short of two minutes: still nothing to do.
    fake_clock_advance(&f.clock, UPDATE_POLICY_DEFAULT_MAX_INTERVAL_MS - 1u);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
    HAC_CHECK_EQ_U64(d.next_deadline_ms, UPDATE_POLICY_DEFAULT_MAX_INTERVAL_MS);

    fake_clock_advance(&f.clock, 1u);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_PERIODIC);
}

// Fresh data that does not move: the line says "just now", a redraw would
// draw the same, and there is no timer at all.
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

// One fresh quantity among stale ones is enough: the picture is current.
static void one_fresh_quantity_stops_the_timer(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    air_reading_t r = stale(reading_at(&f, 620.0f, 22.0f, 45.0f));
    r.co2_ppm.status = VAL_OK;
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);
    update_policy_commit(&f.policy, &r, &d);

    fake_clock_advance(&f.clock, 24u * 3600u * 1000u);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
}

// Nothing carries a number -- every sensor failed: the line says "--", and
// that does not change either.
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

// Past the budget, with stale data that keeps moving: once nothing changes any
// more, the periodic redraw -- the "updated N ago" line's -- is the one that
// comes out full.
static void past_the_budget_the_periodic_redraw_is_the_full_one(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    air_reading_t r = stale(reading_at(&f, 620.0f, 22.0f, 45.0f));
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);
    update_policy_commit(&f.policy, &r, &d);

    float co2 = 620.0f;
    for (uint32_t i = 0u; i <= TEST_MAX_PARTIALS; ++i) {  // one past the budget
        fake_clock_advance(&f.clock, 1000u);
        co2 += 100.0f;
        r = stale(reading_at(&f, co2, 22.0f, 45.0f));
        update_policy_evaluate(&f.policy, &r, &d);
        update_policy_commit(&f.policy, &r, &d);
    }

    fake_clock_advance(&f.clock, UPDATE_POLICY_DEFAULT_MAX_INTERVAL_MS);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_PERIODIC);
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

static void the_periodic_redraw_can_be_switched_off(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    f.policy.cfg.max_interval_ms = 0u;

    const air_reading_t r = stale(reading_at(&f, 620.0f, 22.0f, 45.0f));
    update_decision_t first;
    update_policy_evaluate(&f.policy, &r, &first);
    update_policy_commit(&f.policy, &r, &first);

    fake_clock_advance(&f.clock, 24u * 3600u * 1000u);
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);

    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
    HAC_CHECK_EQ_U64(d.next_deadline_ms, UPDATE_POLICY_NO_DEADLINE);
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
    HAC_CHECK_EQ_U64(cfg.max_interval_ms, 120000u);
    HAC_CHECK_EQ_U64(cfg.min_redraw_interval_ms, 5000u);  // one sensor cycle, not a requirement
}

int main(void)
{
    HAC_RUN(first_frame_is_a_full_refresh);
    HAC_RUN(a_crossed_threshold_triggers_a_partial_refresh);
    HAC_RUN(a_threshold_not_crossed_leaves_the_panel_alone);
    HAC_RUN(the_temperature_and_humidity_thresholds_apply_too);
    HAC_RUN(a_stale_reading_redraws_on_the_periodic_timeout);
    HAC_RUN(a_fresh_unchanged_reading_is_never_redrawn_on_a_timer);
    HAC_RUN(one_fresh_quantity_stops_the_timer);
    HAC_RUN(with_no_number_on_the_screen_there_is_no_timer);
    HAC_RUN(a_status_change_alone_reaches_the_glass);
    HAC_RUN(the_partial_budget_forces_a_full_refresh);
    HAC_RUN(past_the_budget_it_refreshes_partially_until_a_full_refresh_is_allowed);
    HAC_RUN(a_budget_spent_exactly_waits_for_the_next_change);
    HAC_RUN(past_the_budget_the_periodic_redraw_is_the_full_one);
    HAC_RUN(past_the_budget_with_fresh_data_the_next_change_is_the_full_one);
    HAC_RUN(at_the_firmwares_pace_the_budget_is_never_passed);
    HAC_RUN(a_second_redraw_within_one_cycle_waits_for_the_cycle);
    HAC_RUN(a_status_change_waits_no_longer_than_one_cycle);
    HAC_RUN(past_the_budget_is_bounded_by_the_interval);
    HAC_RUN(a_panel_without_a_partial_mode_defers_until_a_full_refresh_is_allowed);
    HAC_RUN(a_panel_without_a_partial_mode_only_ever_refreshes_fully);
    HAC_RUN(the_periodic_redraw_can_be_switched_off);
    HAC_RUN(limits_come_from_the_display_port);
    HAC_RUN(the_defaults_are_the_thresholds_from_the_requirements);
    return hac_test_summary();
}
