// SPDX-License-Identifier: Apache-2.0
//
// adp_clock_virtual mirrors fake_clock's contract (see test_fake_clock.c) and
// adds the scale conversion a --time-scale simulator run relies on.

#include "adp_clock_virtual.h"
#include "hac_test.h"

static void starts_at_a_cold_boot(void)
{
    adp_clock_virtual_t vc;
    adp_clock_virtual_init(&vc, 1.0);
    const port_clock_t *clk = adp_clock_virtual_port(&vc);

    HAC_CHECK_EQ_U64(clk->now_ms(clk), 0u);
    HAC_CHECK_EQ_U64(clk->wall_ms(clk), 0u);  // unsynchronized reads as 0
}

static void time_moves_only_when_told(void)
{
    adp_clock_virtual_t vc;
    adp_clock_virtual_init(&vc, 1.0);
    const port_clock_t *clk = adp_clock_virtual_port(&vc);

    adp_clock_virtual_advance(&vc, 1500u);
    HAC_CHECK_EQ_U64(clk->now_ms(clk), 1500u);

    // A full simulated day costs one call, not a day.
    adp_clock_virtual_advance(&vc, 24u * 60u * 60u * 1000u);
    HAC_CHECK_EQ_U64(clk->now_ms(clk), 86401500u);
}

static void delay_advances_virtual_time_instead_of_blocking(void)
{
    adp_clock_virtual_t vc;
    adp_clock_virtual_init(&vc, 1.0);
    const port_clock_t *clk = adp_clock_virtual_port(&vc);

    clk->delay_ms(clk, 200u);
    clk->delay_ms(clk, 800u);

    HAC_CHECK_EQ_U64(clk->now_ms(clk), 1000u);
    HAC_CHECK_EQ_U64(vc.delay_calls, 2u);
    HAC_CHECK_EQ_U64(vc.delayed_total_ms, 1000u);
}

static void scale_converts_real_time_to_virtual_time(void)
{
    adp_clock_virtual_t vc;
    adp_clock_virtual_init(&vc, 60.0);  // a simulated day in 24 real seconds

    HAC_CHECK_EQ_U64(adp_clock_virtual_scale_ms(&vc, 1000u), 60000u);
    HAC_CHECK_EQ_U64(adp_clock_virtual_scale_ms(&vc, 0u), 0u);
    HAC_CHECK(adp_clock_virtual_scale(&vc) == 60.0);

    // scale_ms() is pure: it never touches now_ms on its own.
    HAC_CHECK_EQ_U64(adp_clock_virtual_port(&vc)->now_ms(adp_clock_virtual_port(&vc)), 0u);

    adp_clock_virtual_set_scale(&vc, 3600.0);
    HAC_CHECK(adp_clock_virtual_scale(&vc) == 3600.0);
    HAC_CHECK_EQ_U64(adp_clock_virtual_scale_ms(&vc, 24u), 86400u);
}

static void a_non_positive_scale_falls_back_to_one_to_one(void)
{
    adp_clock_virtual_t vc;
    adp_clock_virtual_init(&vc, 0.0);
    HAC_CHECK(adp_clock_virtual_scale(&vc) == 1.0);

    adp_clock_virtual_init(&vc, -5.0);
    HAC_CHECK(adp_clock_virtual_scale(&vc) == 1.0);

    adp_clock_virtual_set_scale(&vc, 0.0);
    HAC_CHECK(adp_clock_virtual_scale(&vc) == 1.0);
}

static void wall_clock_tracks_the_monotonic_clock_once_synced(void)
{
    adp_clock_virtual_t vc;
    adp_clock_virtual_init(&vc, 1.0);
    const port_clock_t *clk = adp_clock_virtual_port(&vc);

    adp_clock_virtual_advance(&vc, 5000u);
    HAC_CHECK_EQ_U64(clk->wall_ms(clk), 0u);  // still unsynchronized

    const uint64_t epoch_ms = 1757000000000u;
    adp_clock_virtual_set_wall(&vc, epoch_ms);
    HAC_CHECK_EQ_U64(clk->wall_ms(clk), epoch_ms);

    adp_clock_virtual_advance(&vc, 60000u);
    HAC_CHECK_EQ_U64(clk->wall_ms(clk), epoch_ms + 60000u);
    HAC_CHECK_EQ_U64(clk->now_ms(clk), 65000u);  // monotonic is unaffected
}

static void reinit_models_the_restart_after_deep_sleep(void)
{
    adp_clock_virtual_t vc;
    adp_clock_virtual_init(&vc, 60.0);
    adp_clock_virtual_advance(&vc, 3600000u);
    adp_clock_virtual_set_wall(&vc, 1757000000000u);

    adp_clock_virtual_init(&vc, 60.0);  // deep sleep is a cold start
    const port_clock_t *clk = adp_clock_virtual_port(&vc);

    HAC_CHECK_EQ_U64(clk->now_ms(clk), 0u);
    HAC_CHECK_EQ_U64(clk->wall_ms(clk), 0u);
    HAC_CHECK_EQ_U64(vc.delay_calls, 0u);
}

int main(void)
{
    HAC_RUN(starts_at_a_cold_boot);
    HAC_RUN(time_moves_only_when_told);
    HAC_RUN(delay_advances_virtual_time_instead_of_blocking);
    HAC_RUN(scale_converts_real_time_to_virtual_time);
    HAC_RUN(a_non_positive_scale_falls_back_to_one_to_one);
    HAC_RUN(wall_clock_tracks_the_monotonic_clock_once_synced);
    HAC_RUN(reinit_models_the_restart_after_deep_sleep);
    return hac_test_summary();
}
