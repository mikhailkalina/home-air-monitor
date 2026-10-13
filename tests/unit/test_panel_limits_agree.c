// SPDX-License-Identifier: Apache-2.0
//
// The device and the simulator must report the same two panel limits through
// port_display, or core/app/update_policy.c runs one policy on the glass and
// another on the PC -- and every simulator run stops saying anything about the
// panel. The values are written down twice by design (board_config.h belongs
// to the ESP32 build, epd_emulation.h to the host's), so this is the check
// that they still agree.
//
// board_config.h is included here as plain data: it depends on nothing but
// <stdint.h>, and nothing else of the ESP32 platform reaches the host build.

#include "board_config.h"
#include "epd_emulation.h"
#include "hac_test.h"

static void the_partial_refresh_budget_agrees(void)
{
    HAC_CHECK_EQ_U64(BOARD_EPD_MAX_PARTIAL_REFRESHES_BEFORE_FULL,
                     EPD_SIM_MAX_PARTIAL_REFRESHES_BEFORE_FULL);
}

static void the_minimum_full_refresh_interval_agrees(void)
{
    HAC_CHECK_EQ_U64(BOARD_EPD_MIN_FULL_REFRESH_INTERVAL_MS, EPD_SIM_MIN_FULL_REFRESH_INTERVAL_MS);
}

int main(void)
{
    HAC_RUN(the_partial_refresh_budget_agrees);
    HAC_RUN(the_minimum_full_refresh_interval_agrees);
    return hac_test_summary();
}
