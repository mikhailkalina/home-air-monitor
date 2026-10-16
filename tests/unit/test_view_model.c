// SPDX-License-Identifier: Apache-2.0
//
// What the home screen says, checked as data rather than as pixels. The golden
// test in tests/golden covers the drawing; this covers the decisions behind it,
// above all the requirement that every value_status_t is representable and
// none of them renders as a blank.

#include <string.h>

#include "domain/civil_time.h"
#include "hac_test.h"
#include "ui/view_model.h"

// For the cases that are not about the clock: no reading here carries a wall
// time, so the zone is never consulted.
static const time_zone_t k_utc = {0, 0, {0u, 0u, 0u, 0}, {0u, 0u, 0u, 0}};

// Vilnius, the device's zone, for the "updated" line (main.c).
#define VILNIUS_STANDARD_MIN 120

// 2026-10-16T11:07:30Z: 14:07:30 in Vilnius, summer time.
#define WALL_MS 1792148850000u
#define MINUTE_MS 60000u
#define HOUR_MS 3600000u

static bool text_is(const vm_field_view_t *f, const char *expected)
{
    return strcmp(f->text, expected) == 0;
}

static air_reading_t normal_reading(uint64_t ts_ms)
{
    air_reading_t r;

    air_reading_init(&r);
    r.co2_ppm = measurement_ok(738.0f, ts_ms, 1u);
    r.temperature_c = measurement_ok(23.44f, ts_ms, 2u);
    r.humidity_rh = measurement_ok(45.2f, ts_ms, 2u);
    r.pressure_hpa = measurement_ok(1012.8f, ts_ms, 2u);
    r.iaq = measurement_ok(84.0f, ts_ms, 2u);
    r.battery_percent = measurement_ok(87.0f, ts_ms, 3u);
    r.net_connected = true;
    return r;
}

static void a_normal_reading_becomes_numbers_and_units(void)
{
    view_model_t vm;
    const air_reading_t r = normal_reading(1000u);

    view_model_build(&vm, &r, 1000u, &k_utc);

    HAC_CHECK(text_is(&vm.fields[VM_FIELD_CO2], "738"));
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_TEMPERATURE], "23.4"));
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_HUMIDITY], "45"));
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_PRESSURE], "1013"));
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_IAQ], "84"));
    HAC_CHECK(text_is(&vm.battery, "87"));

    HAC_CHECK(strcmp(vm.fields[VM_FIELD_CO2].unit, "ppm") == 0);
    HAC_CHECK(strcmp(vm.fields[VM_FIELD_HUMIDITY].unit, "%") == 0);
    HAC_CHECK(vm.net_connected);
    HAC_CHECK(strcmp(vm.status_text, "all sensors OK") == 0);
    HAC_CHECK_EQ_INT(vm.error_count, 0);
}

static void negative_and_rounded_values_format_correctly(void)
{
    view_model_t vm;
    air_reading_t r;

    air_reading_init(&r);
    r.temperature_c = measurement_ok(-4.25f, 0u, 2u);
    r.humidity_rh = measurement_ok(0.4f, 0u, 2u);
    view_model_build(&vm, &r, 0u, &k_utc);

    // Half away from zero, and a leading zero rather than a bare point.
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_TEMPERATURE], "-4.3"));
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_HUMIDITY], "0"));

    air_reading_init(&r);
    r.temperature_c = measurement_ok(0.42f, 0u, 2u);
    view_model_build(&vm, &r, 0u, &k_utc);
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_TEMPERATURE], "0.4"));
}

// The requirement this file exists for: five statuses, five distinct
// treatments, and never an empty string.
static void every_status_has_its_own_treatment(void)
{
    const value_status_t statuses[] = {
        VAL_OK, VAL_STALE, VAL_ERROR, VAL_WARMUP, VAL_UNAVAILABLE,
    };
    const size_t count = sizeof(statuses) / sizeof(statuses[0]);

    for (size_t i = 0u; i < count; ++i) {
        view_model_t vm;
        air_reading_t r;

        air_reading_init(&r);
        if (statuses[i] == VAL_OK || statuses[i] == VAL_STALE) {
            r.co2_ppm = measurement_ok(738.0f, 0u, 1u);
            r.co2_ppm.status = statuses[i];
        } else {
            r.co2_ppm = measurement_status(statuses[i], 0u, 1u);
        }
        view_model_build(&vm, &r, 0u, &k_utc);

        const vm_field_view_t *co2 = &vm.fields[VM_FIELD_CO2];
        HAC_CHECK(co2->text[0] != '\0');  // never a blank field
        HAC_CHECK_EQ_INT(co2->status, statuses[i]);
        HAC_CHECK_EQ_INT(co2->mark, view_model_mark_for(statuses[i]));
        HAC_CHECK_EQ_INT(co2->ink, view_model_ink_for(statuses[i]));
    }

    // The marks are distinct from one another: no two statuses share an icon.
    for (size_t i = 0u; i < count; ++i) {
        for (size_t j = i + 1u; j < count; ++j) {
            HAC_CHECK(view_model_mark_for(statuses[i]) != view_model_mark_for(statuses[j]));
        }
    }

    // A stale value keeps its number, faded; the others say what is wrong.
    view_model_t vm;
    air_reading_t r;

    air_reading_init(&r);
    r.co2_ppm = measurement_ok(738.0f, 0u, 1u);
    r.co2_ppm.status = VAL_STALE;
    view_model_build(&vm, &r, 0u, &k_utc);
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_CO2], "738"));
    HAC_CHECK(vm.fields[VM_FIELD_CO2].has_value);
    HAC_CHECK(vm.fields[VM_FIELD_CO2].ink != VM_INK_PRIMARY);

    air_reading_init(&r);
    r.co2_ppm = measurement_status(VAL_ERROR, 0u, 1u);
    view_model_build(&vm, &r, 0u, &k_utc);
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_CO2], "ERR"));
    HAC_CHECK(!vm.fields[VM_FIELD_CO2].has_value);

    air_reading_init(&r);
    r.co2_ppm = measurement_status(VAL_WARMUP, 0u, 1u);
    view_model_build(&vm, &r, 0u, &k_utc);
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_CO2], "warm"));

    air_reading_init(&r);
    view_model_build(&vm, &r, 0u, &k_utc);  // a zeroed reading: everything unavailable
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_CO2], "n/a"));
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_IAQ], "n/a"));
    HAC_CHECK(text_is(&vm.battery, "n/a"));
}

static void faults_are_counted_and_summarized(void)
{
    view_model_t vm;
    air_reading_t r = normal_reading(0u);

    r.co2_ppm = measurement_status(VAL_ERROR, 0u, 1u);
    view_model_build(&vm, &r, 0u, &k_utc);
    HAC_CHECK_EQ_INT(vm.error_count, 1);
    HAC_CHECK(strcmp(vm.status_text, "1 sensor error") == 0);

    r.iaq = measurement_status(VAL_ERROR, 0u, 2u);
    view_model_build(&vm, &r, 0u, &k_utc);
    HAC_CHECK_EQ_INT(vm.error_count, 2);
    HAC_CHECK(strcmp(vm.status_text, "2 sensor errors") == 0);

    // An error outranks a warm-up, which outranks staleness: the summary line
    // reports the worst thing on the screen.
    air_reading_init(&r);
    r.co2_ppm = measurement_status(VAL_WARMUP, 0u, 1u);
    r.temperature_c = measurement_ok(22.0f, 0u, 2u);
    r.temperature_c.status = VAL_STALE;
    view_model_build(&vm, &r, 0u, &k_utc);
    HAC_CHECK_EQ_INT(vm.warmup_count, 1);
    HAC_CHECK_EQ_INT(vm.stale_count, 1);
    HAC_CHECK(strcmp(vm.status_text, "sensors warming up") == 0);

    air_reading_init(&r);
    r.temperature_c = measurement_ok(22.0f, 0u, 2u);
    r.temperature_c.status = VAL_STALE;
    view_model_build(&vm, &r, 0u, &k_utc);
    HAC_CHECK(strcmp(vm.status_text, "data is stale") == 0);
}

static void the_co2_band_follows_the_requirements(void)
{
    const struct {
        float ppm;
        vm_level_t level;
        bool warning;
    } cases[] = {
        {400.0f, VM_LEVEL_GOOD, false},      {799.0f, VM_LEVEL_GOOD, false},
        {800.0f, VM_LEVEL_MODERATE, false},  {1199.0f, VM_LEVEL_MODERATE, false},
        {1200.0f, VM_LEVEL_POOR, true},      {1999.0f, VM_LEVEL_POOR, true},
        {2000.0f, VM_LEVEL_VERY_POOR, true}, {5000.0f, VM_LEVEL_VERY_POOR, true},
    };

    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        view_model_t vm;
        air_reading_t r;

        air_reading_init(&r);
        r.co2_ppm = measurement_ok(cases[i].ppm, 0u, 1u);
        view_model_build(&vm, &r, 0u, &k_utc);

        HAC_CHECK_EQ_INT(vm.co2_level, cases[i].level);
        HAC_CHECK_EQ_INT(vm.co2_warning, cases[i].warning);
        HAC_CHECK(strcmp(vm.co2_level_text, view_model_level_text(cases[i].level)) == 0);
    }

    // No CO2 reading: no band, and above all no warning claimed on no evidence.
    view_model_t vm;
    air_reading_t r;
    air_reading_init(&r);
    r.co2_ppm = measurement_status(VAL_ERROR, 0u, 1u);
    view_model_build(&vm, &r, 0u, &k_utc);
    HAC_CHECK_EQ_INT(vm.co2_level, VM_LEVEL_UNKNOWN);
    HAC_CHECK(!vm.co2_warning);
    HAC_CHECK(strcmp(vm.co2_level_text, "--") == 0);
}

// --- the "updated" line ---------------------------------------------------------

// A reading of two values measured `co2_age_ms` and `t_age_ms` before the
// wall time WALL_MS, which was read at monotonic `now_ms`.
static air_reading_t measured_before(uint64_t now_ms, uint64_t co2_age_ms, uint64_t t_age_ms)
{
    air_reading_t r;
    air_reading_init(&r);
    r.co2_ppm = measurement_ok(738.0f, now_ms - co2_age_ms, 1u);
    r.temperature_c = measurement_ok(22.0f, now_ms - t_age_ms, 2u);
    r.wall_time_ms = WALL_MS;
    return r;
}

// The local minute the freshest value was measured, rounded down -- not an
// age, and the same whether it is fresh or stale.
static void the_update_line_is_when_the_freshest_value_was_measured(void)
{
    const time_zone_t vilnius = time_zone_eu(VILNIUS_STANDARD_MIN);
    const uint64_t now = 1000000000u;
    view_model_t vm;

    air_reading_t r = measured_before(now, 90000u, 6000u);  // 14:06:00 and 14:07:24
    view_model_build(&vm, &r, now, &vilnius);
    HAC_CHECK(vm.has_updated);
    HAC_CHECK(strcmp(vm.updated_text, "14:07") == 0);

    // Rounded down: 14:07:00 exactly, and a millisecond before it.
    r = measured_before(now, 90000u, 30000u);
    view_model_build(&vm, &r, now, &vilnius);
    HAC_CHECK(strcmp(vm.updated_text, "14:07") == 0);
    r = measured_before(now, 90000u, 30001u);
    view_model_build(&vm, &r, now, &vilnius);
    HAC_CHECK(strcmp(vm.updated_text, "14:06") == 0);

    // Stale: the same line, an hour back, and nothing in it says "ago".
    r = measured_before(now, 2u * HOUR_MS, HOUR_MS + 10u * MINUTE_MS);
    r.co2_ppm.status = VAL_STALE;
    r.temperature_c.status = VAL_STALE;
    view_model_build(&vm, &r, now, &vilnius);
    HAC_CHECK(strcmp(vm.updated_text, "12:57") == 0);
}

// The same reading drawn later says the same: the line does not move with
// the clock, so nothing has to redraw it.
static void the_update_line_does_not_change_with_time_alone(void)
{
    const time_zone_t vilnius = time_zone_eu(VILNIUS_STANDARD_MIN);
    const uint64_t now = 1000000000u;
    view_model_t vm;
    air_reading_t r = measured_before(now, 90000u, 6000u);
    view_model_build(&vm, &r, now, &vilnius);
    HAC_CHECK(strcmp(vm.updated_text, "14:07") == 0);

    r.wall_time_ms += 3u * HOUR_MS;  // the loops read both clocks together
    view_model_build(&vm, &r, now + 3u * HOUR_MS, &vilnius);
    HAC_CHECK(strcmp(vm.updated_text, "14:07") == 0);
}

// From an earlier local day, the date goes in front -- local, not UTC.
static void an_update_from_an_earlier_day_carries_its_date(void)
{
    const time_zone_t vilnius = time_zone_eu(VILNIUS_STANDARD_MIN);
    const uint64_t now = 1000000000u;
    view_model_t vm;

    // 2026-10-16T21:00:30Z is 00:00:30 on the 17th in Vilnius; a value from
    // a minute earlier is from 23:59 on the 16th.
    air_reading_t r = measured_before(now, 61000u, 61000u);
    r.wall_time_ms = 1792184430000u;
    view_model_build(&vm, &r, now, &vilnius);
    HAC_CHECK(strcmp(vm.updated_text, "16.10 23:59") == 0);

    // The same instant read in UTC is still the 16th: no date.
    view_model_build(&vm, &r, now, &k_utc);
    HAC_CHECK(strcmp(vm.updated_text, "20:59") == 0);

    // Across a year end, and a leap day.
    r.wall_time_ms = 1798754400000u + 30000u;  // 2027-01-01T00:00:30 in Vilnius
    view_model_build(&vm, &r, now, &vilnius);
    HAC_CHECK(strcmp(vm.updated_text, "31.12 23:59") == 0);
    r.wall_time_ms = 1835474400000u + 30000u;  // 2028-03-01T00:00:30 in Vilnius
    view_model_build(&vm, &r, now, &vilnius);
    HAC_CHECK(strcmp(vm.updated_text, "29.02 23:59") == 0);

    // Days back, the day and month are still all it says.
    r = measured_before(now, 3u * 24u * HOUR_MS, 3u * 24u * HOUR_MS);
    r.co2_ppm.status = VAL_STALE;
    r.temperature_c.status = VAL_STALE;
    view_model_build(&vm, &r, now, &vilnius);
    HAC_CHECK(strcmp(vm.updated_text, "13.10 14:07") == 0);
}

// The hour that summer time repeats or skips is shown as the zone has it.
static void the_update_line_follows_summer_time(void)
{
    const time_zone_t vilnius = time_zone_eu(VILNIUS_STANDARD_MIN);
    const uint64_t now = 1000000000u;
    view_model_t vm;
    air_reading_t r = measured_before(now, 0u, 0u);

    r.wall_time_ms = 1792889999000u;  // 2026-10-25T00:59:59Z
    view_model_build(&vm, &r, now, &vilnius);
    HAC_CHECK(strcmp(vm.updated_text, "03:59") == 0);
    r.wall_time_ms = 1792890000000u;  // 01:00:00Z: clocks go back
    view_model_build(&vm, &r, now, &vilnius);
    HAC_CHECK(strcmp(vm.updated_text, "03:00") == 0);
}

// No wall time, or nothing carrying a number: "--:--", in the faint ink.
static void without_a_time_to_give_the_line_says_so(void)
{
    const time_zone_t vilnius = time_zone_eu(VILNIUS_STANDARD_MIN);
    const uint64_t now = 1000000000u;
    view_model_t vm;

    air_reading_t r = measured_before(now, 6000u, 6000u);
    r.wall_time_ms = 0u;  // the clock is not set
    view_model_build(&vm, &r, now, &vilnius);
    HAC_CHECK(!vm.has_updated);
    HAC_CHECK(strcmp(vm.updated_text, "--:--") == 0);

    air_reading_init(&r);  // nothing has ever been read
    r.wall_time_ms = WALL_MS;
    view_model_build(&vm, &r, now, &vilnius);
    HAC_CHECK(!vm.has_updated);
    HAC_CHECK(strcmp(vm.updated_text, "--:--") == 0);

    r.co2_ppm = measurement_status(VAL_ERROR, now, 1u);  // a failure carries no number
    r.temperature_c = measurement_status(VAL_WARMUP, now, 2u);
    view_model_build(&vm, &r, now, &vilnius);
    HAC_CHECK(strcmp(vm.updated_text, "--:--") == 0);

    // A value stamped after the wall time was read, or before the clock's
    // origin: its age is unknown, and no time is made up for it.
    r = measured_before(now, 6000u, 6000u);
    r.co2_ppm.ts_ms = now + 1u;
    r.temperature_c.ts_ms = now + 1u;
    view_model_build(&vm, &r, now, &vilnius);
    HAC_CHECK(strcmp(vm.updated_text, "--:--") == 0);
}

// --- the IAQ cell without BSEC ------------------------------------------------

// What the device reads with no BSEC linked: a BME68x reporting gas
// resistance, and nothing reporting IAQ.
static air_reading_t without_bsec(float gas_ohm, value_status_t gas_status)
{
    air_reading_t r = normal_reading(1000u);
    r.iaq = measurement_unavailable();
    if (gas_status == VAL_OK || gas_status == VAL_STALE) {
        r.gas_resistance_ohm = measurement_ok(gas_ohm, 1000u, 2u);
        r.gas_resistance_ohm.status = gas_status;
    } else {
        r.gas_resistance_ohm = measurement_status(gas_status, 1000u, 2u);
    }
    return r;
}

static void without_iaq_the_cell_shows_gas_resistance(void)
{
    view_model_t vm;
    const air_reading_t r = without_bsec(182000.0f, VAL_OK);
    view_model_build(&vm, &r, 1000u, &k_utc);

    const vm_field_view_t *cell = &vm.fields[VM_FIELD_IAQ];
    HAC_CHECK(strcmp(cell->label, "Gas") == 0);
    HAC_CHECK(strcmp(cell->unit, "kOhm") == 0);
    HAC_CHECK(text_is(cell, "182"));
    HAC_CHECK(cell->has_value);
    HAC_CHECK_EQ_INT(cell->status, VAL_OK);
    HAC_CHECK_EQ_INT(cell->mark, VM_MARK_NONE);
    HAC_CHECK_EQ_INT(cell->ink, VM_INK_PRIMARY);
    HAC_CHECK(strcmp(vm.status_text, "all sensors OK") == 0);
}

static void gas_resistance_reads_in_kohm_with_a_decimal_only_when_small(void)
{
    const struct {
        float ohm;
        const char *text;
    } cases[] = {
        {8400.0f, "8.4"}, {9940.0f, "9.9"},       {9960.0f, "10"},    {10000.0f, "10"},
        {87000.0f, "87"}, {182499.0f, "182"},     {182500.0f, "183"}, {1234567.0f, "1235"},
        {950.0f, "1.0"},  {12946860.0f, "12947"},
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        view_model_t vm;
        const air_reading_t r = without_bsec(cases[i].ohm, VAL_OK);
        view_model_build(&vm, &r, 1000u, &k_utc);
        HAC_CHECK(text_is(&vm.fields[VM_FIELD_IAQ], cases[i].text));
    }
}

static void gas_resistance_brings_its_own_status(void)
{
    view_model_t vm;
    air_reading_t r = without_bsec(0.0f, VAL_WARMUP);  // the BME68x burning in
    view_model_build(&vm, &r, 1000u, &k_utc);
    const vm_field_view_t *cell = &vm.fields[VM_FIELD_IAQ];
    HAC_CHECK(strcmp(cell->label, "Gas") == 0);
    HAC_CHECK(text_is(cell, "warm"));
    HAC_CHECK_EQ_INT(cell->mark, VM_MARK_WARMUP);
    HAC_CHECK_EQ_INT(vm.warmup_count, 1);
    HAC_CHECK(strcmp(vm.status_text, "sensors warming up") == 0);

    r = without_bsec(0.0f, VAL_ERROR);  // not marked valid and heater-stable
    view_model_build(&vm, &r, 1000u, &k_utc);
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_IAQ], "ERR"));
    HAC_CHECK_EQ_INT(vm.error_count, 1);
    HAC_CHECK(strcmp(vm.status_text, "1 sensor error") == 0);

    r = without_bsec(87000.0f, VAL_STALE);
    view_model_build(&vm, &r, 1000u, &k_utc);
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_IAQ], "87"));
    HAC_CHECK_EQ_INT(vm.fields[VM_FIELD_IAQ].ink, VM_INK_STALE);
    HAC_CHECK_EQ_INT(vm.stale_count, 1);
}

static void an_iaq_that_is_provided_keeps_the_cell(void)
{
    view_model_t vm;
    air_reading_t r = without_bsec(87000.0f, VAL_OK);

    // BSEC is there but not settled, or failing: still IAQ's cell.
    r.iaq = measurement_status(VAL_WARMUP, 1000u, 2u);
    view_model_build(&vm, &r, 1000u, &k_utc);
    HAC_CHECK(strcmp(vm.fields[VM_FIELD_IAQ].label, "IAQ") == 0);
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_IAQ], "warm"));

    r.iaq = measurement_status(VAL_ERROR, 1000u, 2u);
    view_model_build(&vm, &r, 1000u, &k_utc);
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_IAQ], "ERR"));
    HAC_CHECK(strcmp(vm.fields[VM_FIELD_IAQ].unit, "") == 0);

    r.iaq = measurement_ok(84.0f, 1000u, 2u);
    r.gas_resistance_ohm = measurement_status(VAL_ERROR, 1000u, 2u);
    view_model_build(&vm, &r, 1000u, &k_utc);
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_IAQ], "84"));
    HAC_CHECK_EQ_INT(vm.error_count, 0);  // the gas is not on the screen

    // Neither: the cell is IAQ's, and says it is not available.
    r.iaq = measurement_unavailable();
    r.gas_resistance_ohm = measurement_unavailable();
    view_model_build(&vm, &r, 1000u, &k_utc);
    HAC_CHECK(strcmp(vm.fields[VM_FIELD_IAQ].label, "IAQ") == 0);
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_IAQ], "n/a"));
}

static void the_update_line_follows_the_gas_in_the_cell(void)
{
    const time_zone_t vilnius = time_zone_eu(VILNIUS_STANDARD_MIN);
    const uint64_t now = 1000000000u;
    view_model_t vm;
    air_reading_t r;
    air_reading_init(&r);
    r.wall_time_ms = WALL_MS;                                            // 14:07:30
    r.co2_ppm = measurement_ok(738.0f, now - 2u * HOUR_MS, 1u);          // 12:07:30
    r.gas_resistance_ohm = measurement_ok(87000.0f, now - HOUR_MS, 2u);  // 13:07:30
    r.co2_ppm.status = VAL_STALE;
    r.gas_resistance_ohm.status = VAL_STALE;
    view_model_build(&vm, &r, now, &vilnius);
    HAC_CHECK(strcmp(vm.updated_text, "13:07") == 0);

    r.iaq = measurement_ok(84.0f, now - 3u * HOUR_MS, 2u);  // now IAQ's cell: the gas is off screen
    r.iaq.status = VAL_STALE;
    view_model_build(&vm, &r, now, &vilnius);
    HAC_CHECK(strcmp(vm.updated_text, "12:07") == 0);
}

// --- the IAQ band and the accuracy mark -----------------------------------------

static void an_iaq_number_has_its_band_under_it(void)
{
    const struct {
        float iaq;
        iaq_level_t level;
        const char *text;
    } cases[] = {
        {50.0f, IAQ_LEVEL_EXCELLENT, "Excellent"},
        {84.0f, IAQ_LEVEL_GOOD, "Good"},
        {128.0f, IAQ_LEVEL_LIGHTLY_POLLUTED, "Fair"},
        {175.0f, IAQ_LEVEL_MODERATELY_POLLUTED, "Moderate"},
        {260.0f, IAQ_LEVEL_POLLUTED, "Poor"},
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        view_model_t vm;
        air_reading_t r = normal_reading(1000u);
        r.iaq = measurement_ok(cases[i].iaq, 1000u, 2u);
        view_model_build(&vm, &r, 1000u, &k_utc);
        HAC_CHECK_EQ_INT(vm.iaq_level, cases[i].level);
        HAC_CHECK(strcmp(vm.fields[VM_FIELD_IAQ].unit, cases[i].text) == 0);
        HAC_CHECK(strcmp(view_model_iaq_level_text(cases[i].level), cases[i].text) == 0);
    }
    HAC_CHECK(strcmp(view_model_iaq_level_text(IAQ_LEVEL_UNKNOWN), "") == 0);
}

// The band of a whole number, written out from the bounds rather than taken
// from air_quality.c, which is what is under test.
static iaq_level_t band_of_whole_number(int shown)
{
    if (shown <= AIR_QUALITY_IAQ_EXCELLENT_MAX) {
        return IAQ_LEVEL_EXCELLENT;
    }
    if (shown <= AIR_QUALITY_IAQ_GOOD_MAX) {
        return IAQ_LEVEL_GOOD;
    }
    if (shown <= AIR_QUALITY_IAQ_LIGHTLY_POLLUTED_MAX) {
        return IAQ_LEVEL_LIGHTLY_POLLUTED;
    }
    if (shown <= AIR_QUALITY_IAQ_MODERATELY_POLLUTED_MAX) {
        return IAQ_LEVEL_MODERATELY_POLLUTED;
    }
    return IAQ_LEVEL_POLLUTED;
}

// The number and the band under it never disagree: across the whole range,
// in steps finer than the rounding, the band is the one the number shown
// falls in.
static void the_band_always_matches_the_number_shown(void)
{
    for (int step = 0; step <= 6000; ++step) {
        const float iaq = (float)step * 0.05f;
        view_model_t vm;
        air_reading_t r = normal_reading(1000u);
        r.iaq = measurement_ok(iaq, 1000u, 2u);
        view_model_build(&vm, &r, 1000u, &k_utc);

        int shown = 0;
        for (const char *p = vm.fields[VM_FIELD_IAQ].text; *p != '\0'; ++p) {
            shown = shown * 10 + (*p - '0');
        }
        const iaq_level_t expected = band_of_whole_number(shown);
        if (vm.iaq_level != expected) {
            HAC_CHECK(vm.iaq_level == expected);  // one failure, not six thousand
            return;
        }
    }
}

static void without_an_iaq_number_there_is_no_band(void)
{
    view_model_t vm;
    air_reading_t r = normal_reading(1000u);

    r.iaq = measurement_status(VAL_WARMUP, 1000u, 2u);
    r.iaq.accuracy = ACC_UNRELIABLE;
    view_model_build(&vm, &r, 1000u, &k_utc);
    HAC_CHECK_EQ_INT(vm.iaq_level, IAQ_LEVEL_UNKNOWN);
    HAC_CHECK(strcmp(vm.fields[VM_FIELD_IAQ].unit, "") == 0);
    HAC_CHECK_EQ_INT(vm.fields[VM_FIELD_IAQ].mark, VM_MARK_WARMUP);  // not the question mark

    // Gas in the cell: its unit, and no IAQ band.
    r.iaq = measurement_unavailable();
    r.gas_resistance_ohm = measurement_ok(153000.0f, 1000u, 2u);
    view_model_build(&vm, &r, 1000u, &k_utc);
    HAC_CHECK_EQ_INT(vm.iaq_level, IAQ_LEVEL_UNKNOWN);
    HAC_CHECK(strcmp(vm.fields[VM_FIELD_IAQ].unit, "kOhm") == 0);

    // A stale number keeps its band.
    r.iaq = measurement_ok(128.0f, 1000u, 2u);
    r.iaq.status = VAL_STALE;
    view_model_build(&vm, &r, 1000u, &k_utc);
    HAC_CHECK(strcmp(vm.fields[VM_FIELD_IAQ].unit, "Fair") == 0);
}

static void a_number_not_yet_calibrated_gets_the_question_mark(void)
{
    view_model_t vm;
    air_reading_t r = normal_reading(1000u);
    r.iaq.accuracy = ACC_LOW;
    view_model_build(&vm, &r, 1000u, &k_utc);

    const vm_field_view_t *cell = &vm.fields[VM_FIELD_IAQ];
    HAC_CHECK_EQ_INT(cell->mark, VM_MARK_LOW_ACCURACY);
    HAC_CHECK_EQ_INT(cell->ink, VM_INK_PRIMARY);  // a reading, not a weaker one
    HAC_CHECK(text_is(cell, "84"));
    HAC_CHECK(strcmp(cell->unit, "Good") == 0);
    HAC_CHECK_EQ_INT(vm.low_accuracy_count, 1);
    HAC_CHECK(strcmp(vm.status_text, "IAQ not calibrated") == 0);
}

static void a_calibrated_or_unrated_number_has_no_mark(void)
{
    const value_accuracy_t accuracies[] = {ACC_NOT_REPORTED, ACC_MEDIUM, ACC_HIGH};
    for (size_t i = 0u; i < sizeof(accuracies) / sizeof(accuracies[0]); ++i) {
        view_model_t vm;
        air_reading_t r = normal_reading(1000u);
        r.iaq.accuracy = accuracies[i];
        view_model_build(&vm, &r, 1000u, &k_utc);
        HAC_CHECK_EQ_INT(vm.fields[VM_FIELD_IAQ].mark, VM_MARK_NONE);
        HAC_CHECK_EQ_INT(vm.low_accuracy_count, 0);
        HAC_CHECK(strcmp(vm.status_text, "all sensors OK") == 0);
    }
}

// The footer gives worse news first, and any other status keeps its own mark.
static void worse_news_comes_before_calibration(void)
{
    view_model_t vm;
    air_reading_t r = normal_reading(1000u);
    r.iaq.accuracy = ACC_LOW;

    r.iaq.status = VAL_STALE;
    view_model_build(&vm, &r, 1000u, &k_utc);
    HAC_CHECK_EQ_INT(vm.fields[VM_FIELD_IAQ].mark, VM_MARK_STALE);
    HAC_CHECK_EQ_INT(vm.low_accuracy_count, 0);
    HAC_CHECK(strcmp(vm.status_text, "data is stale") == 0);

    r.iaq.status = VAL_OK;
    r.temperature_c = measurement_status(VAL_WARMUP, 1000u, 2u);
    view_model_build(&vm, &r, 1000u, &k_utc);
    HAC_CHECK_EQ_INT(vm.fields[VM_FIELD_IAQ].mark, VM_MARK_LOW_ACCURACY);
    HAC_CHECK(strcmp(vm.status_text, "sensors warming up") == 0);

    r.co2_ppm = measurement_status(VAL_ERROR, 1000u, 1u);
    view_model_build(&vm, &r, 1000u, &k_utc);
    HAC_CHECK(strcmp(vm.status_text, "1 sensor error") == 0);
}

static void several_numbers_not_calibrated_are_counted(void)
{
    view_model_t vm;
    air_reading_t r = normal_reading(1000u);
    r.iaq.accuracy = ACC_LOW;
    r.co2_ppm.accuracy = ACC_LOW;
    view_model_build(&vm, &r, 1000u, &k_utc);
    HAC_CHECK_EQ_INT(vm.low_accuracy_count, 2);
    HAC_CHECK(strcmp(vm.status_text, "2 not calibrated") == 0);
    HAC_CHECK_EQ_INT(vm.fields[VM_FIELD_CO2].mark, VM_MARK_LOW_ACCURACY);

    // One, under the longest label: the footer's shorter name for it.
    air_reading_init(&r);
    r.temperature_c = measurement_ok(22.0f, 1000u, 2u);
    r.temperature_c.accuracy = ACC_LOW;
    view_model_build(&vm, &r, 1000u, &k_utc);
    HAC_CHECK(strcmp(vm.status_text, "Temp. not calibrated") == 0);
    HAC_CHECK(strcmp(vm.fields[VM_FIELD_TEMPERATURE].label, "Temperature") == 0);
}

int main(void)
{
    HAC_RUN(a_normal_reading_becomes_numbers_and_units);
    HAC_RUN(negative_and_rounded_values_format_correctly);
    HAC_RUN(every_status_has_its_own_treatment);
    HAC_RUN(faults_are_counted_and_summarized);
    HAC_RUN(the_co2_band_follows_the_requirements);
    HAC_RUN(the_update_line_is_when_the_freshest_value_was_measured);
    HAC_RUN(the_update_line_does_not_change_with_time_alone);
    HAC_RUN(an_update_from_an_earlier_day_carries_its_date);
    HAC_RUN(the_update_line_follows_summer_time);
    HAC_RUN(without_a_time_to_give_the_line_says_so);
    HAC_RUN(without_iaq_the_cell_shows_gas_resistance);
    HAC_RUN(gas_resistance_reads_in_kohm_with_a_decimal_only_when_small);
    HAC_RUN(gas_resistance_brings_its_own_status);
    HAC_RUN(an_iaq_that_is_provided_keeps_the_cell);
    HAC_RUN(the_update_line_follows_the_gas_in_the_cell);
    HAC_RUN(an_iaq_number_has_its_band_under_it);
    HAC_RUN(the_band_always_matches_the_number_shown);
    HAC_RUN(without_an_iaq_number_there_is_no_band);
    HAC_RUN(a_number_not_yet_calibrated_gets_the_question_mark);
    HAC_RUN(a_calibrated_or_unrated_number_has_no_mark);
    HAC_RUN(worse_news_comes_before_calibration);
    HAC_RUN(several_numbers_not_calibrated_are_counted);
    return hac_test_summary();
}
