// SPDX-License-Identifier: Apache-2.0

#include <math.h>

#include "domain/measurement.h"
#include "fake_clock.h"
#include "hac_test.h"

#define SRC_SCD41 1u
#define SRC_BME68X 2u
#define MAX_AGE_MS 60000u

// --- construction ----------------------------------------------------------

static void a_zero_filled_reading_is_unavailable_not_zero_valued(void)
{
    air_reading_t r;
    air_reading_init(&r);

    HAC_CHECK_EQ_INT(r.co2_ppm.status, VAL_UNAVAILABLE);
    HAC_CHECK_EQ_INT(r.temperature_c.status, VAL_UNAVAILABLE);
    HAC_CHECK_EQ_INT(r.battery_percent.status, VAL_UNAVAILABLE);
    HAC_CHECK(!measurement_has_value(&r.co2_ppm));

    HAC_CHECK(!r.net_connected);
    HAC_CHECK(!r.telemetry_connected);
    HAC_CHECK(!r.charging);
    HAC_CHECK_EQ_INT(r.wifi_rssi_dbm, 0);
    HAC_CHECK_EQ_U64(r.boot_count, 0u);
    HAC_CHECK_EQ_U64(r.wall_time_ms, 0u);
}

static void a_successful_reading_records_value_time_and_source(void)
{
    const measurement_f32_t m = measurement_ok(738.0f, 12345u, SRC_SCD41);

    HAC_CHECK_EQ_INT(m.status, VAL_OK);
    HAC_CHECK_EQ_F32(m.value, 738.0f, 0.0f);
    HAC_CHECK_EQ_U64(m.ts_ms, 12345u);
    HAC_CHECK_EQ_INT(m.source_id, SRC_SCD41);
    HAC_CHECK(measurement_has_value(&m));
}

static void a_non_finite_value_is_a_fault_not_a_reading(void)
{
    const measurement_f32_t nan_m = measurement_ok(NAN, 1000u, SRC_BME68X);
    HAC_CHECK_EQ_INT(nan_m.status, VAL_ERROR);
    HAC_CHECK_EQ_F32(nan_m.value, 0.0f, 0.0f);
    HAC_CHECK(!measurement_has_value(&nan_m));

    const measurement_f32_t inf_m = measurement_ok(INFINITY, 1000u, SRC_BME68X);
    HAC_CHECK_EQ_INT(inf_m.status, VAL_ERROR);

    const measurement_f32_t neg_inf_m = measurement_ok(-INFINITY, 1000u, SRC_BME68X);
    HAC_CHECK_EQ_INT(neg_inf_m.status, VAL_ERROR);
}

static void a_faulted_reading_carries_no_value(void)
{
    const measurement_f32_t warm = measurement_status(VAL_WARMUP, 500u, SRC_SCD41);
    HAC_CHECK_EQ_INT(warm.status, VAL_WARMUP);
    HAC_CHECK_EQ_F32(warm.value, 0.0f, 0.0f);
    HAC_CHECK_EQ_U64(warm.ts_ms, 500u);
    HAC_CHECK(!measurement_has_value(&warm));

    const measurement_f32_t err = measurement_status(VAL_ERROR, 500u, SRC_SCD41);
    HAC_CHECK(!measurement_has_value(&err));

    const measurement_f32_t none = measurement_unavailable();
    HAC_CHECK_EQ_INT(none.status, VAL_UNAVAILABLE);
    HAC_CHECK_EQ_U64(none.ts_ms, 0u);
    HAC_CHECK_EQ_INT(none.source_id, 0);
}

// --- age -------------------------------------------------------------------

static void age_is_the_distance_from_acquisition_to_now(void)
{
    const measurement_f32_t m = measurement_ok(21.5f, 1000u, SRC_BME68X);

    HAC_CHECK_EQ_U64(measurement_age_ms(&m, 1000u), 0u);
    HAC_CHECK_EQ_U64(measurement_age_ms(&m, 1500u), 500u);
    HAC_CHECK_EQ_U64(measurement_age_ms(&m, 61000u), 60000u);
}

static void age_is_unknown_without_an_acquisition(void)
{
    const measurement_f32_t none = measurement_unavailable();

    HAC_CHECK_EQ_U64(measurement_age_ms(&none, 100000u), MEASUREMENT_AGE_UNKNOWN);
}

static void age_is_unknown_when_the_monotonic_clock_restarted(void)
{
    // Taken an hour into the previous power cycle; read back after deep sleep,
    // when now_ms has restarted from 0.
    const measurement_f32_t m = measurement_ok(738.0f, 3600000u, SRC_SCD41);

    HAC_CHECK_EQ_U64(measurement_age_ms(&m, 0u), MEASUREMENT_AGE_UNKNOWN);
    HAC_CHECK_EQ_U64(measurement_age_ms(&m, 3599999u), MEASUREMENT_AGE_UNKNOWN);
}

// --- VAL_STALE derivation --------------------------------------------------

static void staleness_starts_one_millisecond_past_the_limit(void)
{
    const measurement_f32_t m = measurement_ok(738.0f, 1000u, SRC_SCD41);

    HAC_CHECK(!measurement_is_stale(&m, 1000u, MAX_AGE_MS));
    HAC_CHECK(!measurement_is_stale(&m, 1000u + MAX_AGE_MS, MAX_AGE_MS));  // exactly at the limit
    HAC_CHECK(measurement_is_stale(&m, 1001u + MAX_AGE_MS, MAX_AGE_MS));

    // A zero limit is not a special case: anything not taken this millisecond
    // is already too old.
    HAC_CHECK(!measurement_is_stale(&m, 1000u, 0u));
    HAC_CHECK(measurement_is_stale(&m, 1001u, 0u));
}

static void an_unknown_age_is_older_than_any_limit(void)
{
    const measurement_f32_t m = measurement_ok(738.0f, 3600000u, SRC_SCD41);

    HAC_CHECK(measurement_is_stale(&m, 0u, MAX_AGE_MS));
    HAC_CHECK(measurement_is_stale(&m, 0u, UINT32_MAX));
}

static void statuses_without_a_value_are_never_merely_stale(void)
{
    const measurement_f32_t none = measurement_unavailable();
    const measurement_f32_t warm = measurement_status(VAL_WARMUP, 0u, SRC_SCD41);
    const measurement_f32_t err = measurement_status(VAL_ERROR, 0u, SRC_SCD41);

    HAC_CHECK(!measurement_is_stale(&none, 100000000u, MAX_AGE_MS));
    HAC_CHECK(!measurement_is_stale(&warm, 100000000u, MAX_AGE_MS));
    HAC_CHECK(!measurement_is_stale(&err, 100000000u, MAX_AGE_MS));
}

static void apply_age_demotes_ok_and_leaves_everything_else_alone(void)
{
    measurement_f32_t m = measurement_ok(738.0f, 1000u, SRC_SCD41);

    measurement_apply_age(&m, 30000u, MAX_AGE_MS);
    HAC_CHECK_EQ_INT(m.status, VAL_OK);  // still inside the window

    measurement_apply_age(&m, 90000u, MAX_AGE_MS);
    HAC_CHECK_EQ_INT(m.status, VAL_STALE);
    HAC_CHECK_EQ_F32(m.value, 738.0f, 0.0f);  // the number survives; only trust changes
    HAC_CHECK(measurement_has_value(&m));

    measurement_f32_t warm = measurement_status(VAL_WARMUP, 1000u, SRC_SCD41);
    measurement_apply_age(&warm, 90000u, MAX_AGE_MS);
    HAC_CHECK_EQ_INT(warm.status, VAL_WARMUP);

    measurement_f32_t err = measurement_status(VAL_ERROR, 1000u, SRC_SCD41);
    measurement_apply_age(&err, 90000u, MAX_AGE_MS);
    HAC_CHECK_EQ_INT(err.status, VAL_ERROR);

    measurement_f32_t none = measurement_unavailable();
    measurement_apply_age(&none, 90000u, MAX_AGE_MS);
    HAC_CHECK_EQ_INT(none.status, VAL_UNAVAILABLE);
}

static void apply_age_is_idempotent_and_never_revives_a_value(void)
{
    measurement_f32_t m = measurement_ok(738.0f, 1000u, SRC_SCD41);

    measurement_apply_age(&m, 90000u, MAX_AGE_MS);
    measurement_apply_age(&m, 90000u, MAX_AGE_MS);
    HAC_CHECK_EQ_INT(m.status, VAL_STALE);

    // Re-running the derivation at a moment that would look fresh must not
    // hand back a value that already aged out.
    measurement_apply_age(&m, 1000u, MAX_AGE_MS);
    HAC_CHECK_EQ_INT(m.status, VAL_STALE);
    HAC_CHECK(measurement_is_stale(&m, 1000u, MAX_AGE_MS));
}

// --- the whole reading, driven by the clock port ---------------------------

static void a_dead_sensor_goes_stale_while_a_live_one_stays_fresh(void)
{
    fake_clock_t fc;
    fake_clock_init(&fc);
    const port_clock_t *clk = fake_clock_port(&fc);

    air_reading_t r;
    air_reading_init(&r);

    // Both sensors report at t = 10 s.
    fake_clock_advance(&fc, 10000u);
    r.co2_ppm = measurement_ok(738.0f, clk->now_ms(clk), SRC_SCD41);
    r.temperature_c = measurement_ok(21.5f, clk->now_ms(clk), SRC_BME68X);
    r.humidity_rh = measurement_ok(45.0f, clk->now_ms(clk), SRC_BME68X);

    // Five minutes later the SCD41 has stopped answering; the BME68x has not.
    fake_clock_advance(&fc, 5u * 60u * 1000u);
    r.temperature_c = measurement_ok(21.7f, clk->now_ms(clk), SRC_BME68X);
    r.humidity_rh = measurement_ok(45.4f, clk->now_ms(clk), SRC_BME68X);

    air_reading_apply_age(&r, clk->now_ms(clk), MAX_AGE_MS);

    HAC_CHECK_EQ_INT(r.co2_ppm.status, VAL_STALE);
    HAC_CHECK_EQ_INT(r.temperature_c.status, VAL_OK);
    HAC_CHECK_EQ_INT(r.humidity_rh.status, VAL_OK);
    HAC_CHECK_EQ_INT(r.pressure_hpa.status, VAL_UNAVAILABLE);  // never reported at all

    // The last good CO2 number is still there to display next to the indicator.
    HAC_CHECK_EQ_F32(r.co2_ppm.value, 738.0f, 0.0f);
    HAC_CHECK_EQ_INT(r.co2_ppm.source_id, SRC_SCD41);
}

static void values_from_before_deep_sleep_do_not_look_fresh_after_it(void)
{
    fake_clock_t fc;
    fake_clock_init(&fc);
    const port_clock_t *clk = fake_clock_port(&fc);

    air_reading_t r;
    air_reading_init(&r);

    fake_clock_advance(&fc, 60u * 60u * 1000u);  // an hour of uptime
    r.co2_ppm = measurement_ok(738.0f, clk->now_ms(clk), SRC_SCD41);
    r.temperature_c = measurement_ok(21.5f, clk->now_ms(clk), SRC_BME68X);
    r.battery_percent = measurement_ok(74.0f, clk->now_ms(clk), 0u);

    // Deep sleep, then a cold boot: the retained reading survives, the
    // monotonic clock does not.
    fake_clock_init(&fc);
    clk = fake_clock_port(&fc);
    fake_clock_advance(&fc, 120u);  // a little way into the new power cycle

    air_reading_apply_age(&r, clk->now_ms(clk), MAX_AGE_MS);

    HAC_CHECK_EQ_INT(r.co2_ppm.status, VAL_STALE);
    HAC_CHECK_EQ_INT(r.temperature_c.status, VAL_STALE);
    HAC_CHECK_EQ_INT(r.battery_percent.status, VAL_STALE);
}

int main(void)
{
    HAC_RUN(a_zero_filled_reading_is_unavailable_not_zero_valued);
    HAC_RUN(a_successful_reading_records_value_time_and_source);
    HAC_RUN(a_non_finite_value_is_a_fault_not_a_reading);
    HAC_RUN(a_faulted_reading_carries_no_value);

    HAC_RUN(age_is_the_distance_from_acquisition_to_now);
    HAC_RUN(age_is_unknown_without_an_acquisition);
    HAC_RUN(age_is_unknown_when_the_monotonic_clock_restarted);

    HAC_RUN(staleness_starts_one_millisecond_past_the_limit);
    HAC_RUN(an_unknown_age_is_older_than_any_limit);
    HAC_RUN(statuses_without_a_value_are_never_merely_stale);
    HAC_RUN(apply_age_demotes_ok_and_leaves_everything_else_alone);
    HAC_RUN(apply_age_is_idempotent_and_never_revives_a_value);

    HAC_RUN(a_dead_sensor_goes_stale_while_a_live_one_stays_fresh);
    HAC_RUN(values_from_before_deep_sleep_do_not_look_fresh_after_it);
    return hac_test_summary();
}
