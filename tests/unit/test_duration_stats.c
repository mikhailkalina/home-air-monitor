// SPDX-License-Identifier: Apache-2.0
//
// The statistics the panel calibration mode reports
// (apps/firmware_esp32/main/duration_stats.c, compiled here unmodified).

#include "duration_stats.h"
#include "hac_test.h"

static void an_empty_series_reports_zeroes(void)
{
    duration_stats_t s;
    duration_stats_init(&s);

    HAC_CHECK_EQ_U64(s.count, 0u);
    HAC_CHECK_EQ_U64(s.min_us, 0u);
    HAC_CHECK_EQ_U64(s.max_us, 0u);
    HAC_CHECK_EQ_U64(duration_stats_mean_us(&s), 0u);
}

static void one_sample_is_its_own_min_max_and_mean(void)
{
    duration_stats_t s;
    duration_stats_init(&s);
    duration_stats_add(&s, 401234u);

    HAC_CHECK_EQ_U64(s.count, 1u);
    HAC_CHECK_EQ_U64(s.min_us, 401234u);
    HAC_CHECK_EQ_U64(s.max_us, 401234u);
    HAC_CHECK_EQ_U64(duration_stats_mean_us(&s), 401234u);
}

// The first sample must not be allowed to stick as the minimum just because
// the struct started at zero, nor as the maximum because it came first.
static void min_and_max_follow_the_samples_in_either_order(void)
{
    duration_stats_t falling;
    duration_stats_init(&falling);
    duration_stats_add(&falling, 900u);
    duration_stats_add(&falling, 500u);
    duration_stats_add(&falling, 100u);
    HAC_CHECK_EQ_U64(falling.min_us, 100u);
    HAC_CHECK_EQ_U64(falling.max_us, 900u);

    duration_stats_t rising;
    duration_stats_init(&rising);
    duration_stats_add(&rising, 100u);
    duration_stats_add(&rising, 500u);
    duration_stats_add(&rising, 900u);
    HAC_CHECK_EQ_U64(rising.min_us, 100u);
    HAC_CHECK_EQ_U64(rising.max_us, 900u);
}

// A zero-length sample is a real (if implausible) measurement, not "unset".
static void a_zero_sample_counts_as_the_minimum(void)
{
    duration_stats_t s;
    duration_stats_init(&s);
    duration_stats_add(&s, 300u);
    duration_stats_add(&s, 0u);

    HAC_CHECK_EQ_U64(s.min_us, 0u);
    HAC_CHECK_EQ_U64(s.max_us, 300u);
    HAC_CHECK_EQ_U64(duration_stats_mean_us(&s), 150u);
}

static void the_mean_rounds_halves_up_and_the_rest_to_nearest(void)
{
    duration_stats_t half;
    duration_stats_init(&half);
    duration_stats_add(&half, 1u);
    duration_stats_add(&half, 2u);
    HAC_CHECK_EQ_U64(duration_stats_mean_us(&half), 2u);  // 1.5

    duration_stats_t third;
    duration_stats_init(&third);
    duration_stats_add(&third, 1u);
    duration_stats_add(&third, 1u);
    duration_stats_add(&third, 2u);
    HAC_CHECK_EQ_U64(duration_stats_mean_us(&third), 1u);  // 1.33

    duration_stats_t two_thirds;
    duration_stats_init(&two_thirds);
    duration_stats_add(&two_thirds, 1u);
    duration_stats_add(&two_thirds, 2u);
    duration_stats_add(&two_thirds, 2u);
    HAC_CHECK_EQ_U64(duration_stats_mean_us(&two_thirds), 2u);  // 1.67
}

// Samples well past a 32-bit microsecond count (~71 minutes each) still sum
// and average exactly: the accumulator is 64-bit throughout.
static void samples_beyond_32_bits_do_not_overflow(void)
{
    const uint64_t big = (uint64_t)UINT32_MAX * 4u;

    duration_stats_t s;
    duration_stats_init(&s);
    duration_stats_add(&s, big);
    duration_stats_add(&s, big);
    duration_stats_add(&s, big);

    HAC_CHECK_EQ_U64(s.sum_us, big * 3u);
    HAC_CHECK_EQ_U64(duration_stats_mean_us(&s), big);
}

int main(void)
{
    HAC_RUN(an_empty_series_reports_zeroes);
    HAC_RUN(one_sample_is_its_own_min_max_and_mean);
    HAC_RUN(min_and_max_follow_the_samples_in_either_order);
    HAC_RUN(a_zero_sample_counts_as_the_minimum);
    HAC_RUN(the_mean_rounds_halves_up_and_the_rest_to_nearest);
    HAC_RUN(samples_beyond_32_bits_do_not_overflow);
    return hac_test_summary();
}
