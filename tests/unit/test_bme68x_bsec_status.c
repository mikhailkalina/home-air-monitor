// SPDX-License-Identifier: Apache-2.0
//
// How BSEC's verdicts on its outputs become value_status_t
// (drivers/bme68x/bme68x_bsec_status.h, docs/adr/0020-bsec-iaq.md). Built in
// every configuration -- it needs nothing of Bosch's -- so the mapping is
// checked in CI, which never has the library.

#include <stdint.h>
#include <string.h>

#include "bme68x_bsec_status.h"
#include "hac_test.h"

static void only_an_unreliable_iaq_is_a_warm_up(void)
{
    HAC_CHECK_EQ_INT(bme68x_bsec_iaq_status(BME68X_BSEC_ACCURACY_UNRELIABLE), VAL_WARMUP);
    HAC_CHECK_EQ_INT(bme68x_bsec_iaq_status(BME68X_BSEC_ACCURACY_LOW), VAL_OK);
    HAC_CHECK_EQ_INT(bme68x_bsec_iaq_status(BME68X_BSEC_ACCURACY_MEDIUM), VAL_OK);
    HAC_CHECK_EQ_INT(bme68x_bsec_iaq_status(BME68X_BSEC_ACCURACY_HIGH), VAL_OK);
}

static void an_accuracy_bsec_does_not_define_is_an_error(void)
{
    HAC_CHECK_EQ_INT(bme68x_bsec_iaq_status(4u), VAL_ERROR);
    HAC_CHECK_EQ_INT(bme68x_bsec_iaq_status(UINT8_MAX), VAL_ERROR);
}

// Bosch's numbering, which the mapping is written against: a library that
// renumbered them would have to change this file too.
static void the_accuracies_are_bosch_numbers(void)
{
    HAC_CHECK_EQ_INT(BME68X_BSEC_ACCURACY_UNRELIABLE, 0);
    HAC_CHECK_EQ_INT(BME68X_BSEC_ACCURACY_LOW, 1);
    HAC_CHECK_EQ_INT(BME68X_BSEC_ACCURACY_MEDIUM, 2);
    HAC_CHECK_EQ_INT(BME68X_BSEC_ACCURACY_HIGH, 3);
}

static void the_accuracy_reaches_the_core_on_its_own_scale(void)
{
    HAC_CHECK_EQ_INT(bme68x_bsec_iaq_accuracy(BME68X_BSEC_ACCURACY_UNRELIABLE), ACC_UNRELIABLE);
    HAC_CHECK_EQ_INT(bme68x_bsec_iaq_accuracy(BME68X_BSEC_ACCURACY_LOW), ACC_LOW);
    HAC_CHECK_EQ_INT(bme68x_bsec_iaq_accuracy(BME68X_BSEC_ACCURACY_MEDIUM), ACC_MEDIUM);
    HAC_CHECK_EQ_INT(bme68x_bsec_iaq_accuracy(BME68X_BSEC_ACCURACY_HIGH), ACC_HIGH);
    HAC_CHECK_EQ_INT(bme68x_bsec_iaq_accuracy(4u), ACC_NOT_REPORTED);
}

// What the screen marks (measurement_low_accuracy()) is exactly what BSEC
// calls unreliable or low.
static void the_screen_marks_what_bsec_calls_unreliable_or_low(void)
{
    for (uint8_t a = 0u; a <= 3u; ++a) {
        measurement_f32_t m = measurement_ok(84.0f, 1000u, 2u);
        m.accuracy = bme68x_bsec_iaq_accuracy(a);
        HAC_CHECK(measurement_low_accuracy(&m) == (a <= BME68X_BSEC_ACCURACY_LOW));
    }
}

static void gas_the_sensor_did_not_mark_valid_is_an_error_whatever_bsec_says(void)
{
    HAC_CHECK_EQ_INT(bme68x_bsec_gas_status(false, true, true), VAL_ERROR);
    HAC_CHECK_EQ_INT(bme68x_bsec_gas_status(false, false, false), VAL_ERROR);
}

static void valid_gas_warms_up_until_both_statuses_say_done(void)
{
    HAC_CHECK_EQ_INT(bme68x_bsec_gas_status(true, false, false), VAL_WARMUP);
    HAC_CHECK_EQ_INT(bme68x_bsec_gas_status(true, true, false), VAL_WARMUP);
    HAC_CHECK_EQ_INT(bme68x_bsec_gas_status(true, false, true), VAL_WARMUP);
    HAC_CHECK_EQ_INT(bme68x_bsec_gas_status(true, true, true), VAL_OK);
}

static void every_accuracy_has_a_name(void)
{
    HAC_CHECK(strcmp(bme68x_bsec_accuracy_name(0u), "unreliable") == 0);
    HAC_CHECK(strcmp(bme68x_bsec_accuracy_name(1u), "low") == 0);
    HAC_CHECK(strcmp(bme68x_bsec_accuracy_name(2u), "medium") == 0);
    HAC_CHECK(strcmp(bme68x_bsec_accuracy_name(3u), "high") == 0);
    HAC_CHECK(strcmp(bme68x_bsec_accuracy_name(4u), "?") == 0);
}

int main(void)
{
    HAC_RUN(only_an_unreliable_iaq_is_a_warm_up);
    HAC_RUN(an_accuracy_bsec_does_not_define_is_an_error);
    HAC_RUN(the_accuracies_are_bosch_numbers);
    HAC_RUN(the_accuracy_reaches_the_core_on_its_own_scale);
    HAC_RUN(the_screen_marks_what_bsec_calls_unreliable_or_low);
    HAC_RUN(gas_the_sensor_did_not_mark_valid_is_an_error_whatever_bsec_says);
    HAC_RUN(valid_gas_warms_up_until_both_statuses_say_done);
    HAC_RUN(every_accuracy_has_a_name);
    return hac_test_summary();
}
