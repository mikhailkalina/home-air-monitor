// SPDX-License-Identifier: Apache-2.0
//
// On-target runner for port_sensor_contract.c: the same body the host runner
// (tests/contract/test_port_sensor_contract.c) runs against the drivers over
// device models, here against the board's real SCD4x and BME68x on adp_i2c.
// Built into the test app with
//
//   cd apps/firmware_esp32/test_app && idf.py -T hac_platform build
//
// The test app never runs EPDiy, whose init is what raises PWR_EN -- and with
// it the switched 3V3 the sensors run from -- in the firmware. So each case
// raises the rail itself first (platform/esp32_t5s3/board_power.h) and waits
// as the firmware does; the drivers then wait their own power-up times.
//
// Time is real here: the suite's advance() waits on adp_clock, so a case
// takes as long as its sensor measures -- about 12 s for the SCD4x in either
// mode, well under a second for the BME68x.

#include <stdbool.h>
#include <stdint.h>

#include "unity.h"

#include "adp_clock.h"
#include "adp_i2c.h"
#include "adp_log.h"
#include "bme68x_source.h"
#include "board_config.h"
#include "board_power.h"
#include "contract_print.h"
#include "port_sensor_contract.h"
#include "scd4x_source.h"

// Ids for the measurements' source_id, as the firmware gives them.
#define SCD4X_SOURCE_ID 1u
#define BME68X_SOURCE_ID 2u

static void report(void *ctx, const char *check, bool ok)
{
    contract_print_check((const contract_print_ctx_t *)ctx, check, ok);
}

static void advance(void *ctx, uint64_t ms)
{
    const port_clock_t *clock = (const port_clock_t *)ctx;
    clock->delay_ms(clock, (ms < UINT32_MAX) ? (uint32_t)ms : UINT32_MAX);
}

typedef struct {
    adp_log_t log;
    adp_clock_t clock;
    adp_i2c_t bus;
} rig_t;

// The rail, the wait the firmware makes after it, the bus.
static void rig_up(rig_t *r)
{
    adp_log_init(&r->log);
    adp_clock_init(&r->clock, adp_log_port(&r->log));
    board_power_switched_3v3_on(adp_log_port(&r->log));
    const port_clock_t *clock = adp_clock_port(&r->clock);
    clock->delay_ms(clock, BOARD_SWITCHED_3V3_SETTLE_MS);
    TEST_ASSERT_EQUAL(HAL_OK, adp_i2c_init(&r->bus, adp_log_port(&r->log)));
}

static unsigned run(sensor_source_t *src, rig_t *r, const char *implementation)
{
    const port_clock_t *clock = adp_clock_port(&r->clock);
    const port_sensor_contract_env_t env = {.advance = advance, .ctx = (void *)clock};
    contract_print_ctx_t ctx = {.port = "port_sensor", .implementation = implementation};
    return port_sensor_contract_run(src, &env, report, &ctx);
}

static void run_scd4x(scd4x_mode_t mode, const char *implementation)
{
    static rig_t rig;
    static scd4x_source_t source;
    rig_up(&rig);

    const scd4x_source_config_t cfg = {
        .bus = adp_i2c_port(&rig.bus),
        .clock = adp_clock_port(&rig.clock),
        .log = adp_log_port(&rig.log),
        .address = BOARD_SCD4X_I2C_ADDRESS,
        .id = SCD4X_SOURCE_ID,
        .mode = mode,
    };
    TEST_ASSERT_EQUAL(HAL_OK, scd4x_source_setup(&source, &cfg));
    const unsigned failures = run(&source.port, &rig, implementation);
    adp_i2c_deinit(&rig.bus);
    TEST_ASSERT_EQUAL_UINT(0u, failures);
}

TEST_CASE("scd4x_source satisfies the port_sensor contract, periodic", "[contract][port_sensor]")
{
    run_scd4x(SCD4X_MODE_PERIODIC, "scd4x_source(periodic)+SCD4x");
}

// Single shot needs an SCD41 or SCD43: on an SCD40 init() refuses it, logs
// "single shot needs an SCD41 or SCD43", and this case fails on
// sensor.init_ok -- which is then the variant's answer, not a defect.
TEST_CASE("scd4x_source satisfies the port_sensor contract, single shot", "[contract][port_sensor]")
{
    run_scd4x(SCD4X_MODE_SINGLE_SHOT, "scd4x_source(single_shot)+SCD4x");
}

TEST_CASE("bme68x_source satisfies the port_sensor contract", "[contract][port_sensor]")
{
    static rig_t rig;
    static bme68x_source_t source;
    rig_up(&rig);

    const bme68x_source_config_t cfg = {
        .bus = adp_i2c_port(&rig.bus),
        .clock = adp_clock_port(&rig.clock),
        .log = adp_log_port(&rig.log),
        .address = BOARD_BME68X_I2C_ADDRESS,
        .id = BME68X_SOURCE_ID,
    };
    TEST_ASSERT_EQUAL(HAL_OK, bme68x_source_setup(&source, &cfg));
    const unsigned failures = run(&source.port, &rig, "bme68x_source+BME68x");
    adp_i2c_deinit(&rig.bus);
    TEST_ASSERT_EQUAL_UINT(0u, failures);
}
