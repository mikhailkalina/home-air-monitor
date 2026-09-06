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

    const update_policy_config_t cfg = update_policy_config_default();
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
    // Nothing to do until the periodic redraw falls due.
    HAC_CHECK_EQ_U64(d.next_deadline_ms, UPDATE_POLICY_DEFAULT_MAX_INTERVAL_MS);
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

static void an_unchanged_reading_redraws_on_the_periodic_timeout(void)
{
    fixture_t f;
    fixture_init(&f, TEST_MIN_FULL_INTERVAL_MS, TEST_MAX_PARTIALS);
    const air_reading_t r = prime(&f, 620.0f, 22.0f, 45.0f);

    update_decision_t d;

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

static void the_minimum_interval_defers_a_due_full_refresh(void)
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
        update_policy_commit(&f.policy, &r, &d);
    }

    // The budget is spent, so this redraw must be full -- but only five
    // seconds have passed since the last full refresh.
    fake_clock_advance(&f.clock, 1000u);
    co2 += 100.0f;
    const air_reading_t r = reading_at(&f, co2, 22.0f, 45.0f);
    update_decision_t d;
    update_policy_evaluate(&f.policy, &r, &d);

    // Deferred, and specifically not downgraded back to a partial refresh.
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
    HAC_CHECK(d.deferred_by_min_interval);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_CO2);
    HAC_CHECK_EQ_U64(d.next_deadline_ms, TEST_MIN_FULL_INTERVAL_MS);

    // Committing a deferred decision must not consume the budget or move the
    // baseline: nothing reached the panel.
    update_policy_commit(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(update_policy_partials_since_full(&f.policy), (int)TEST_MAX_PARTIALS);

    // Once the panel's minimum interval has elapsed, the same reading goes out.
    fake_clock_advance(&f.clock, TEST_MIN_FULL_INTERVAL_MS);
    update_policy_evaluate(&f.policy, &r, &d);
    HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);
    HAC_CHECK(!d.deferred_by_min_interval);
    HAC_CHECK(d.full_forced_by_budget);
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

    const air_reading_t r = prime(&f, 620.0f, 22.0f, 45.0f);

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
}

int main(void)
{
    HAC_RUN(first_frame_is_a_full_refresh);
    HAC_RUN(a_crossed_threshold_triggers_a_partial_refresh);
    HAC_RUN(a_threshold_not_crossed_leaves_the_panel_alone);
    HAC_RUN(the_temperature_and_humidity_thresholds_apply_too);
    HAC_RUN(an_unchanged_reading_redraws_on_the_periodic_timeout);
    HAC_RUN(a_status_change_alone_reaches_the_glass);
    HAC_RUN(the_partial_budget_forces_a_full_refresh);
    HAC_RUN(the_minimum_interval_defers_a_due_full_refresh);
    HAC_RUN(a_panel_without_a_partial_mode_only_ever_refreshes_fully);
    HAC_RUN(the_periodic_redraw_can_be_switched_off);
    HAC_RUN(limits_come_from_the_display_port);
    HAC_RUN(the_defaults_are_the_thresholds_from_the_requirements);
    return hac_test_summary();
}
