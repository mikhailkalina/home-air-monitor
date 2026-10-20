// SPDX-License-Identifier: Apache-2.0
//
// On-target runner for port_input_contract.c: the same body the host runner
// (tests/contract/test_port_input_contract.c) runs against gt911_input over
// the GT911 model, here against adp_input -- the board's GT911 behind its
// interrupt and its task. Built into the test app with
//
//   cd apps/firmware_esp32/test_app && idf.py -T hac_platform build
//
// Nobody's finger is on the panel: what needs one is reported as not
// exercised, and the taps are the board check
// (docs/hardware/board_notes.md, open item "The touch panel on the board").
// What needs no finger runs: init (the wake pulse, the product ID, the
// configuration, the interrupt), a non-blocking poll, no events while
// untouched -- and sleep and wake, phase 5's.
//
// The touch controller runs from VDD3V3, but the sensors on the switched
// 3V3 hold the bus low until PWR_EN is up: the rail first, as in
// test_port_sensor_contract.c.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "adp_clock.h"
#include "adp_i2c.h"
#include "adp_input.h"
#include "adp_log.h"
#include "board_config.h"
#include "board_power.h"
#include "contract_print.h"
#include "port_input_contract.h"

// The touch task's, as the firmware's main.c gives it.
#define TOUCH_TASK_STACK_BYTES 6144u
#define TOUCH_TASK_PRIORITY 6
#define TOUCH_TASK_CORE 0

#define IMPLEMENTATION "adp_input+GT911"

// How long to probe the controller's address while it sleeps.
#define PROBE_TIMEOUT_MS 20u

// How long the touch task may take to find the controller: its wake, a
// second of looking at most, its configuration.
#define READY_TIMEOUT_MS 3000u

typedef struct {
    adp_log_t log;
    adp_clock_t clock;
    adp_i2c_t bus;
    adp_input_t input;
} rig_t;

static rig_t s_rig;

static void report(void *ctx, const char *check, bool ok)
{
    contract_print_check((const contract_print_ctx_t *)ctx, check, ok);
}

static void advance(void *ctx, uint64_t ms)
{
    const port_clock_t *clock = (const port_clock_t *)ctx;
    clock->delay_ms(clock, (ms < UINT32_MAX) ? (uint32_t)ms : UINT32_MAX);
}

static void rig_up(rig_t *r)
{
    adp_log_init(&r->log);
    adp_clock_init(&r->clock, adp_log_port(&r->log));
    board_power_switched_3v3_on(adp_log_port(&r->log));
    const port_clock_t *clock = adp_clock_port(&r->clock);
    clock->delay_ms(clock, BOARD_SWITCHED_3V3_SETTLE_MS);
    TEST_ASSERT_EQUAL(HAL_OK, adp_i2c_init(&r->bus, adp_log_port(&r->log)));
    const adp_input_config_t cfg = {
        .bus = adp_i2c_port(&r->bus),
        .clock = clock,
        .log = adp_log_port(&r->log),
        .task_stack_bytes = TOUCH_TASK_STACK_BYTES,
        .task_priority = TOUCH_TASK_PRIORITY,
        .task_core = TOUCH_TASK_CORE,
    };
    TEST_ASSERT_EQUAL(HAL_OK, adp_input_init(&r->input, &cfg));
    TEST_ASSERT_EQUAL(HAL_OK, adp_input_wait_ready(&r->input, READY_TIMEOUT_MS));
}

static void rig_down(rig_t *r)
{
    adp_input_deinit(&r->input);
    adp_i2c_deinit(&r->bus);
}

TEST_CASE("adp_input satisfies the port_input contract", "[contract][port_input]")
{
    rig_up(&s_rig);
    contract_print_ctx_t ctx = {.port = "port_input", .implementation = IMPLEMENTATION};

    static adp_input_status_t st;
    adp_input_status(&s_rig.input, &st);
    contract_print_check(&ctx, "the controller names itself \"911\"",
                         strcmp(st.info.product_id, "911") == 0);
    printf("contract port_input %s info: address 0x%02X firmware 0x%04X config 0x%02X "
           "x_max %u y_max %u touch_number %u module_switch1 0x%02X trigger %s checksum %s\n",
           IMPLEMENTATION, st.info.address, (unsigned)st.info.firmware_version,
           (unsigned)st.info.config_version, (unsigned)st.info.x_max, (unsigned)st.info.y_max,
           (unsigned)st.info.touch_number, (unsigned)st.info.module_switch1,
           gt911_trigger_name(st.info.trigger), st.info.config_checksum_ok ? "ok" : "BAD");

    const port_clock_t *clock = adp_clock_port(&s_rig.clock);
    const port_input_contract_env_t env = {
        .panel_width = (uint16_t)BOARD_EPD_WIDTH,
        .panel_height = (uint16_t)BOARD_EPD_HEIGHT,
        .clock = clock,
        .advance = advance,
        .press = NULL,
        .drag = NULL,
        .lift = NULL,
        .ctx = (void *)clock,
    };
    const unsigned failures =
        port_input_contract_run(adp_input_port(&s_rig.input), &env, report, &ctx);
    const bool named = strcmp(st.info.product_id, "911") == 0;
    rig_down(&s_rig);
    TEST_ASSERT_TRUE(named);
    TEST_ASSERT_EQUAL_UINT(0u, failures);
}

// Phase 5's: asleep and awake again, with the bus still the sensors'.
// Whether the GT911 refuses its address while it sleeps -- the host model
// assumes so -- is printed as seen, not asserted: it is the board's answer.
TEST_CASE("adp_input puts the controller to sleep and wakes it", "[contract][port_input]")
{
    rig_up(&s_rig);
    contract_print_ctx_t ctx = {.port = "port_input", .implementation = IMPLEMENTATION};
    port_i2c_t *bus = adp_i2c_port(&s_rig.bus);
    static adp_input_status_t st;
    adp_input_status(&s_rig.input, &st);
    const uint8_t address = st.info.address;

    const hal_status_t slept = adp_input_sleep(&s_rig.input);
    contract_print_check(&ctx, "sleep: the command taken", slept == HAL_OK);
    const port_clock_t *clock = adp_clock_port(&s_rig.clock);
    clock->delay_ms(clock, 100u);
    const hal_status_t probe_asleep = bus->write(bus, address, NULL, 0u, PROBE_TIMEOUT_MS);
    printf("contract port_input %s seen: while asleep the address %s (%d)\n", IMPLEMENTATION,
           (probe_asleep == HAL_OK) ? "answers" : "is refused", (int)probe_asleep);

    const int64_t start_ms = (int64_t)clock->now_ms(clock);
    const hal_status_t woke = adp_input_wake(&s_rig.input);
    printf("contract port_input %s seen: wake took %lld ms\n", IMPLEMENTATION,
           (long long)((int64_t)clock->now_ms(clock) - start_ms));
    contract_print_check(&ctx, "wake: the controller answers again", woke == HAL_OK);
    const hal_status_t probe_awake = bus->write(bus, address, NULL, 0u, PROBE_TIMEOUT_MS);
    contract_print_check(&ctx, "wake: its address answers", probe_awake == HAL_OK);
    adp_input_status(&s_rig.input, &st);
    contract_print_check(&ctx, "wake: not asleep", !st.asleep);
    rig_down(&s_rig);
    TEST_ASSERT_EQUAL(HAL_OK, slept);
    TEST_ASSERT_EQUAL(HAL_OK, woke);
    TEST_ASSERT_EQUAL(HAL_OK, probe_awake);
}
