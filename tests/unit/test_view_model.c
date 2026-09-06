// SPDX-License-Identifier: Apache-2.0
//
// What the home screen says, checked as data rather than as pixels. The golden
// test in tests/golden covers the drawing; this covers the decisions behind it,
// above all the requirement that every value_status_t is representable and
// none of them renders as a blank.

#include <string.h>

#include "hac_test.h"
#include "ui/view_model.h"

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

    view_model_build(&vm, &r, 1000u);

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
    view_model_build(&vm, &r, 0u);

    // Half away from zero, and a leading zero rather than a bare point.
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_TEMPERATURE], "-4.3"));
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_HUMIDITY], "0"));

    air_reading_init(&r);
    r.temperature_c = measurement_ok(0.42f, 0u, 2u);
    view_model_build(&vm, &r, 0u);
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
        view_model_build(&vm, &r, 0u);

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
    view_model_build(&vm, &r, 0u);
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_CO2], "738"));
    HAC_CHECK(vm.fields[VM_FIELD_CO2].has_value);
    HAC_CHECK(vm.fields[VM_FIELD_CO2].ink != VM_INK_PRIMARY);

    air_reading_init(&r);
    r.co2_ppm = measurement_status(VAL_ERROR, 0u, 1u);
    view_model_build(&vm, &r, 0u);
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_CO2], "ERR"));
    HAC_CHECK(!vm.fields[VM_FIELD_CO2].has_value);

    air_reading_init(&r);
    r.co2_ppm = measurement_status(VAL_WARMUP, 0u, 1u);
    view_model_build(&vm, &r, 0u);
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_CO2], "warm"));

    air_reading_init(&r);
    view_model_build(&vm, &r, 0u);  // a zeroed reading: everything unavailable
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_CO2], "n/a"));
    HAC_CHECK(text_is(&vm.fields[VM_FIELD_IAQ], "n/a"));
    HAC_CHECK(text_is(&vm.battery, "n/a"));
}

static void faults_are_counted_and_summarized(void)
{
    view_model_t vm;
    air_reading_t r = normal_reading(0u);

    r.co2_ppm = measurement_status(VAL_ERROR, 0u, 1u);
    view_model_build(&vm, &r, 0u);
    HAC_CHECK_EQ_INT(vm.error_count, 1);
    HAC_CHECK(strcmp(vm.status_text, "1 sensor error") == 0);

    r.iaq = measurement_status(VAL_ERROR, 0u, 2u);
    view_model_build(&vm, &r, 0u);
    HAC_CHECK_EQ_INT(vm.error_count, 2);
    HAC_CHECK(strcmp(vm.status_text, "2 sensor errors") == 0);

    // An error outranks a warm-up, which outranks staleness: the summary line
    // reports the worst thing on the screen.
    air_reading_init(&r);
    r.co2_ppm = measurement_status(VAL_WARMUP, 0u, 1u);
    r.temperature_c = measurement_ok(22.0f, 0u, 2u);
    r.temperature_c.status = VAL_STALE;
    view_model_build(&vm, &r, 0u);
    HAC_CHECK_EQ_INT(vm.warmup_count, 1);
    HAC_CHECK_EQ_INT(vm.stale_count, 1);
    HAC_CHECK(strcmp(vm.status_text, "sensors warming up") == 0);

    air_reading_init(&r);
    r.temperature_c = measurement_ok(22.0f, 0u, 2u);
    r.temperature_c.status = VAL_STALE;
    view_model_build(&vm, &r, 0u);
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
        view_model_build(&vm, &r, 0u);

        HAC_CHECK_EQ_INT(vm.co2_level, cases[i].level);
        HAC_CHECK_EQ_INT(vm.co2_warning, cases[i].warning);
        HAC_CHECK(strcmp(vm.co2_level_text, view_model_level_text(cases[i].level)) == 0);
    }

    // No CO2 reading: no band, and above all no warning claimed on no evidence.
    view_model_t vm;
    air_reading_t r;
    air_reading_init(&r);
    r.co2_ppm = measurement_status(VAL_ERROR, 0u, 1u);
    view_model_build(&vm, &r, 0u);
    HAC_CHECK_EQ_INT(vm.co2_level, VM_LEVEL_UNKNOWN);
    HAC_CHECK(!vm.co2_warning);
    HAC_CHECK(strcmp(vm.co2_level_text, "--") == 0);
}

static void the_update_line_reports_the_freshest_quantity(void)
{
    view_model_t vm;
    air_reading_t r;

    air_reading_init(&r);
    r.co2_ppm = measurement_ok(738.0f, 1000u, 1u);        // 90 s old
    r.temperature_c = measurement_ok(22.0f, 85000u, 2u);  // 6 s old

    view_model_build(&vm, &r, 91000u);
    HAC_CHECK(vm.has_updated);
    HAC_CHECK(strcmp(vm.updated_text, "6 s") == 0);

    view_model_build(&vm, &r, 400000u);  // co2 315 s, temperature 315 s
    HAC_CHECK(strcmp(vm.updated_text, "5 min") == 0);

    view_model_build(&vm, &r, 8000000u);
    HAC_CHECK(strcmp(vm.updated_text, "2 h") == 0);

    // Nothing has ever been read: say so rather than showing "0 s".
    air_reading_init(&r);
    view_model_build(&vm, &r, 5000u);
    HAC_CHECK(!vm.has_updated);
    HAC_CHECK(strcmp(vm.updated_text, "--") == 0);
}

int main(void)
{
    HAC_RUN(a_normal_reading_becomes_numbers_and_units);
    HAC_RUN(negative_and_rounded_values_format_correctly);
    HAC_RUN(every_status_has_its_own_treatment);
    HAC_RUN(faults_are_counted_and_summarized);
    HAC_RUN(the_co2_band_follows_the_requirements);
    HAC_RUN(the_update_line_reports_the_freshest_quantity);
    return hac_test_summary();
}
