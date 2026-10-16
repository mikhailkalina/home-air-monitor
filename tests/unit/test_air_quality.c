// SPDX-License-Identifier: Apache-2.0
//
// The IAQ's bands (core/domain/air_quality.h,
// docs/adr/0021-measurement-accuracy-and-iaq-levels.md) at their boundaries,
// judged on the number the screen shows. That the screen's number and its
// band never disagree over the whole range is test_view_model.c's to check,
// where the number is formatted.

#include <math.h>

#include "domain/air_quality.h"
#include "domain/measurement.h"
#include "hac_test.h"

static iaq_level_t level_of(float iaq)
{
    const measurement_f32_t m = measurement_ok(iaq, 1000u, 2u);
    return air_quality_iaq_level(&m);
}

static void the_bands_are_the_owners(void)
{
    HAC_CHECK_EQ_INT(AIR_QUALITY_IAQ_EXCELLENT_MAX, 50);
    HAC_CHECK_EQ_INT(AIR_QUALITY_IAQ_GOOD_MAX, 100);
    HAC_CHECK_EQ_INT(AIR_QUALITY_IAQ_LIGHTLY_POLLUTED_MAX, 150);
    HAC_CHECK_EQ_INT(AIR_QUALITY_IAQ_MODERATELY_POLLUTED_MAX, 200);
}

static void each_upper_bound_belongs_to_its_band(void)
{
    HAC_CHECK_EQ_INT(level_of(0.0f), IAQ_LEVEL_EXCELLENT);
    HAC_CHECK_EQ_INT(level_of(50.0f), IAQ_LEVEL_EXCELLENT);
    HAC_CHECK_EQ_INT(level_of(51.0f), IAQ_LEVEL_GOOD);
    HAC_CHECK_EQ_INT(level_of(100.0f), IAQ_LEVEL_GOOD);
    HAC_CHECK_EQ_INT(level_of(101.0f), IAQ_LEVEL_LIGHTLY_POLLUTED);
    HAC_CHECK_EQ_INT(level_of(150.0f), IAQ_LEVEL_LIGHTLY_POLLUTED);
    HAC_CHECK_EQ_INT(level_of(151.0f), IAQ_LEVEL_MODERATELY_POLLUTED);
    HAC_CHECK_EQ_INT(level_of(200.0f), IAQ_LEVEL_MODERATELY_POLLUTED);
    HAC_CHECK_EQ_INT(level_of(201.0f), IAQ_LEVEL_POLLUTED);
    HAC_CHECK_EQ_INT(level_of(500.0f), IAQ_LEVEL_POLLUTED);
}

// Between the whole numbers, the band follows the rounding the screen does:
// 50.49 reads "50", 50.5 reads "51".
static void between_whole_numbers_the_band_follows_the_rounding(void)
{
    HAC_CHECK_EQ_INT(level_of(50.49f), IAQ_LEVEL_EXCELLENT);
    HAC_CHECK_EQ_INT(level_of(50.5f), IAQ_LEVEL_GOOD);
    HAC_CHECK_EQ_INT(level_of(100.49f), IAQ_LEVEL_GOOD);
    HAC_CHECK_EQ_INT(level_of(100.5f), IAQ_LEVEL_LIGHTLY_POLLUTED);
    HAC_CHECK_EQ_INT(level_of(150.49f), IAQ_LEVEL_LIGHTLY_POLLUTED);
    HAC_CHECK_EQ_INT(level_of(150.5f), IAQ_LEVEL_MODERATELY_POLLUTED);
    HAC_CHECK_EQ_INT(level_of(200.49f), IAQ_LEVEL_MODERATELY_POLLUTED);
    HAC_CHECK_EQ_INT(level_of(200.5f), IAQ_LEVEL_POLLUTED);
}

static void a_negative_index_is_excellent(void)
{
    HAC_CHECK_EQ_INT(level_of(-3.0f), IAQ_LEVEL_EXCELLENT);
}

static void without_a_number_there_is_no_band(void)
{
    const measurement_f32_t none = measurement_unavailable();
    const measurement_f32_t warm = measurement_status(VAL_WARMUP, 1000u, 2u);
    const measurement_f32_t error = measurement_status(VAL_ERROR, 1000u, 2u);
    const measurement_f32_t nan = measurement_ok(NAN, 1000u, 2u);  // VAL_ERROR
    HAC_CHECK_EQ_INT(air_quality_iaq_level(&none), IAQ_LEVEL_UNKNOWN);
    HAC_CHECK_EQ_INT(air_quality_iaq_level(&warm), IAQ_LEVEL_UNKNOWN);
    HAC_CHECK_EQ_INT(air_quality_iaq_level(&error), IAQ_LEVEL_UNKNOWN);
    HAC_CHECK_EQ_INT(air_quality_iaq_level(&nan), IAQ_LEVEL_UNKNOWN);
}

// A stale number is still shown, so it still has its band.
static void a_stale_number_keeps_its_band(void)
{
    measurement_f32_t m = measurement_ok(128.0f, 1000u, 2u);
    m.status = VAL_STALE;
    HAC_CHECK_EQ_INT(air_quality_iaq_level(&m), IAQ_LEVEL_LIGHTLY_POLLUTED);
}

int main(void)
{
    HAC_RUN(the_bands_are_the_owners);
    HAC_RUN(each_upper_bound_belongs_to_its_band);
    HAC_RUN(between_whole_numbers_the_band_follows_the_rounding);
    HAC_RUN(a_negative_index_is_excellent);
    HAC_RUN(without_a_number_there_is_no_band);
    HAC_RUN(a_stale_number_keeps_its_band);
    return hac_test_summary();
}
