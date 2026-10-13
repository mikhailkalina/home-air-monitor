// SPDX-License-Identifier: Apache-2.0
//
// On-target runner for port_i2c_contract.c: the same body the host runner
// (tests/contract/test_port_i2c_contract.c) runs against the fake bus, here
// against adp_i2c on the board's real bus, with the PCF8563 RTC as the
// known-present slave. Built into the test app with
//
//   cd apps/firmware_esp32/test_app && idf.py -T hac_platform build
//
// The faults are real: adp_i2c's fault-injection functions bit-bang a read
// from the RTC and leave it holding SDA low, and take SCL away from the
// controller and hold it low. Expect the driver's own error lines
// ("i2c.master: I2C transaction unexpected nack detected", "I2C hardware
// timeout detected", ...) in the log while this runs -- they are the faults
// being provoked, not failures. Only the "contract port_i2c ..." lines and
// Unity's summary are the result.

#include <stdbool.h>

#include "unity.h"

#include "adp_clock.h"
#include "adp_i2c.h"
#include "adp_log.h"
#include "board_config.h"
#include "contract_print.h"
#include "port_i2c_contract.h"

// The register the suite points the RTC at and reads back: Control_status_1,
// the first register (PCF8563 datasheet, register overview). Reading any
// PCF8563 register is side-effect free, and the suite never writes it more
// than this one pointer byte.
#define RTC_SAFE_REGISTER 0x00u

static void report(void *ctx, const char *check, bool ok)
{
    contract_print_check((const contract_print_ctx_t *)ctx, check, ok);
}

static bool hang(void *ctx)
{
    return adp_i2c_inject_stuck_slave((adp_i2c_t *)ctx, BOARD_RTC_I2C_ADDRESS) == HAL_OK;
}

static bool stall(void *ctx)
{
    return adp_i2c_hold_scl_low((adp_i2c_t *)ctx) == HAL_OK;
}

static void end_stall(void *ctx)
{
    (void)adp_i2c_release_scl((adp_i2c_t *)ctx);
}

static bool sda_held_low(void *ctx)
{
    bool sda_high = false;
    bool scl_high = false;
    adp_i2c_lines((adp_i2c_t *)ctx, &sda_high, &scl_high);
    return !sda_high;
}

TEST_CASE("adp_i2c satisfies the port_i2c contract", "[contract][port_i2c]")
{
    static adp_log_t log;
    static adp_clock_t clock;
    static adp_i2c_t bus;
    adp_log_init(&log);
    adp_clock_init(&clock, adp_log_port(&log));
    TEST_ASSERT_EQUAL(HAL_OK, adp_i2c_init(&bus, adp_log_port(&log)));

    const port_i2c_contract_env_t env = {
        .clock = adp_clock_port(&clock),
        .present_addr = BOARD_RTC_I2C_ADDRESS,
        .present_reg = RTC_SAFE_REGISTER,
        .absent_addr = BOARD_I2C_ABSENT_TEST_ADDRESS,
        .hang_bus = hang,
        .stall_bus = stall,
        .end_stall = end_stall,
        .sda_held_low = sda_held_low,
        .ctx = &bus,
    };

    contract_print_ctx_t ctx = {.port = "port_i2c", .implementation = "adp_i2c"};
    const unsigned failures = port_i2c_contract_run(adp_i2c_port(&bus), &env, report, &ctx);
    adp_i2c_deinit(&bus);
    TEST_ASSERT_EQUAL_UINT(0u, failures);
}
