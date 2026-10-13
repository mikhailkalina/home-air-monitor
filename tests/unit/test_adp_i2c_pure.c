// SPDX-License-Identifier: Apache-2.0
//
// platform/esp32_t5s3/adp_i2c_pure.c, compiled on the host from where the
// firmware compiles it (see tests/CMakeLists.txt).

#include "adp_i2c_pure.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hac_test.h"

// The board's tick rate (CONFIG_FREERTOS_HZ's default; sdkconfig.defaults
// does not change it) and a 1 kHz one, to show the arithmetic is not tied to
// 10 ms ticks.
#define TICK_HZ_BOARD 100u
#define TICK_HZ_1K 1000u

// What FreeRTOS's pdMS_TO_TICKS() does to whatever the adapter hands the
// driver: truncate.
static uint64_t ms_to_ticks_truncating(uint32_t ms, uint32_t hz)
{
    return (uint64_t)ms * hz / 1000u;
}

static void a_passed_deadline_starts_no_wait(void)
{
    HAC_CHECK_EQ_U64(adp_i2c_driver_wait_ms(0u, TICK_HZ_BOARD), 0u);
}

static void a_sub_tick_deadline_still_waits_two_whole_ticks(void)
{
    // 1 us and 1 ms are both under one 10 ms tick: one tick to cover them,
    // one more for the tick already under way.
    HAC_CHECK_EQ_U64(adp_i2c_driver_wait_ms(1u, TICK_HZ_BOARD), 20u);
    HAC_CHECK_EQ_U64(adp_i2c_driver_wait_ms(1000u, TICK_HZ_BOARD), 20u);
    HAC_CHECK_EQ_U64(adp_i2c_driver_wait_ms(9999u, TICK_HZ_BOARD), 20u);
}

static void tick_boundaries_round_up_then_add_one(void)
{
    HAC_CHECK_EQ_U64(adp_i2c_driver_wait_ms(10000u, TICK_HZ_BOARD), 20u);  // exactly one tick
    HAC_CHECK_EQ_U64(adp_i2c_driver_wait_ms(10001u, TICK_HZ_BOARD), 30u);  // just over
    HAC_CHECK_EQ_U64(adp_i2c_driver_wait_ms(50000u, TICK_HZ_BOARD), 60u);
}

static void a_partial_millisecond_counts_as_a_whole_one(void)
{
    HAC_CHECK_EQ_U64(adp_i2c_driver_wait_ms(1001u, TICK_HZ_1K), 3u);  // 2 ms + 1 tick
    HAC_CHECK_EQ_U64(adp_i2c_driver_wait_ms(1000u, TICK_HZ_1K), 2u);
}

static void the_driver_never_waits_fewer_ticks_than_the_deadline_needs(void)
{
    // The property the arithmetic exists for, over every remaining time from
    // 1 us to 1 s at both tick rates: after the driver truncates the value
    // back to ticks, it still waits at least ceil(remaining / tick) + 1.
    const uint32_t rates[] = {TICK_HZ_BOARD, TICK_HZ_1K, 300u};
    bool all_ok = true;
    for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++) {
        const uint32_t hz = rates[r];
        for (uint64_t us = 1u; us <= 1000000u; us += 997u) {
            const uint32_t wait = adp_i2c_driver_wait_ms(us, hz);
            const uint64_t needed = (us * hz + 999999u) / 1000000u + 1u;
            if (ms_to_ticks_truncating(wait, hz) < needed) {
                all_ok = false;
            }
        }
    }
    HAC_CHECK(all_ok);
}

static void an_enormous_deadline_is_clamped_to_what_the_driver_accepts(void)
{
    HAC_CHECK_EQ_U64(adp_i2c_driver_wait_ms(UINT64_MAX / 2000u, TICK_HZ_BOARD),
                     ADP_I2C_DRIVER_WAIT_MAX_MS);
}

static void each_probe_outcome_maps_to_its_port_error(void)
{
    HAC_CHECK_EQ_INT(adp_i2c_classify_failure(ADP_I2C_PROBE_NOT_RUN), HAL_ERR_TIMEOUT);
    HAC_CHECK_EQ_INT(adp_i2c_classify_failure(ADP_I2C_PROBE_ACK), HAL_ERR_IO);
    HAC_CHECK_EQ_INT(adp_i2c_classify_failure(ADP_I2C_PROBE_NACK), HAL_ERR_NOT_FOUND);
    HAC_CHECK_EQ_INT(adp_i2c_classify_failure(ADP_I2C_PROBE_TIMEOUT), HAL_ERR_TIMEOUT);
    HAC_CHECK_EQ_INT(adp_i2c_classify_failure(ADP_I2C_PROBE_OTHER), HAL_ERR_IO);
}

int main(void)
{
    HAC_RUN(a_passed_deadline_starts_no_wait);
    HAC_RUN(a_sub_tick_deadline_still_waits_two_whole_ticks);
    HAC_RUN(tick_boundaries_round_up_then_add_one);
    HAC_RUN(a_partial_millisecond_counts_as_a_whole_one);
    HAC_RUN(the_driver_never_waits_fewer_ticks_than_the_deadline_needs);
    HAC_RUN(an_enormous_deadline_is_clamped_to_what_the_driver_accepts);
    HAC_RUN(each_probe_outcome_maps_to_its_port_error);
    return hac_test_summary();
}
