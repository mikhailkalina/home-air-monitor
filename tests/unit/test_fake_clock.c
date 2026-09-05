// SPDX-License-Identifier: Apache-2.0
//
// The fake clock is the foundation every later test stands on, so its own
// behaviour is pinned here.

#include "fake_clock.h"
#include "hac_test.h"

static void starts_at_a_cold_boot(void)
{
    fake_clock_t fc;
    fake_clock_init(&fc);
    const port_clock_t *clk = fake_clock_port(&fc);

    HAC_CHECK_EQ_U64(clk->now_ms(clk), 0u);
    HAC_CHECK_EQ_U64(clk->wall_ms(clk), 0u);  // unsynchronized reads as 0
    HAC_CHECK_EQ_U64(fc.delayed_total_ms, 0u);
}

static void time_moves_only_when_the_test_moves_it(void)
{
    fake_clock_t fc;
    fake_clock_init(&fc);
    const port_clock_t *clk = fake_clock_port(&fc);

    fake_clock_advance(&fc, 1500u);
    HAC_CHECK_EQ_U64(clk->now_ms(clk), 1500u);

    fake_clock_advance(&fc, 0u);
    HAC_CHECK_EQ_U64(clk->now_ms(clk), 1500u);

    // A full day costs one call, not a day.
    fake_clock_advance(&fc, 24u * 60u * 60u * 1000u);
    HAC_CHECK_EQ_U64(clk->now_ms(clk), 86401500u);
}

static void delay_advances_virtual_time_instead_of_blocking(void)
{
    fake_clock_t fc;
    fake_clock_init(&fc);
    const port_clock_t *clk = fake_clock_port(&fc);

    clk->delay_ms(clk, 200u);
    clk->delay_ms(clk, 800u);

    HAC_CHECK_EQ_U64(clk->now_ms(clk), 1000u);
    HAC_CHECK_EQ_U64(fc.delay_calls, 2u);
    HAC_CHECK_EQ_U64(fc.delayed_total_ms, 1000u);
}

static void wall_clock_tracks_the_monotonic_clock_once_synced(void)
{
    fake_clock_t fc;
    fake_clock_init(&fc);
    const port_clock_t *clk = fake_clock_port(&fc);

    fake_clock_advance(&fc, 5000u);
    HAC_CHECK_EQ_U64(clk->wall_ms(clk), 0u);  // still unsynchronized

    const uint64_t epoch_ms = 1757000000000u;  // some point in real time
    fake_clock_set_wall(&fc, epoch_ms);
    HAC_CHECK_EQ_U64(clk->wall_ms(clk), epoch_ms);

    fake_clock_advance(&fc, 60000u);
    HAC_CHECK_EQ_U64(clk->wall_ms(clk), epoch_ms + 60000u);
    HAC_CHECK_EQ_U64(clk->now_ms(clk), 65000u);  // monotonic is unaffected
}

static void reinit_models_the_restart_after_deep_sleep(void)
{
    fake_clock_t fc;
    fake_clock_init(&fc);
    const port_clock_t *clk = fake_clock_port(&fc);

    fake_clock_advance(&fc, 3600000u);
    fake_clock_set_wall(&fc, 1757000000000u);
    clk->delay_ms(clk, 10u);

    fake_clock_init(&fc);  // deep sleep is a cold start

    clk = fake_clock_port(&fc);
    HAC_CHECK_EQ_U64(clk->now_ms(clk), 0u);
    HAC_CHECK_EQ_U64(clk->wall_ms(clk), 0u);
    HAC_CHECK_EQ_U64(fc.delay_calls, 0u);
}

int main(void)
{
    HAC_RUN(starts_at_a_cold_boot);
    HAC_RUN(time_moves_only_when_the_test_moves_it);
    HAC_RUN(delay_advances_virtual_time_instead_of_blocking);
    HAC_RUN(wall_clock_tracks_the_monotonic_clock_once_synced);
    HAC_RUN(reinit_models_the_restart_after_deep_sleep);
    return hac_test_summary();
}
