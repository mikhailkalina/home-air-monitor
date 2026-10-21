// SPDX-License-Identifier: Apache-2.0
//
// On-target runner for port_net_contract.c: the same body the host runner
// (tests/contract/test_port_net_contract.c) runs against the simulator's
// network, here against adp_net -- ESP-IDF's Wi-Fi station on the board,
// joining the network sdkconfig.credentials names (phase 4e,
// docs/adr/0032-wifi-on-the-board.md). Built into the test app with
//
//   cd apps/firmware_esp32/test_app && idf.py -T hac_platform build
//
// An image without credentials has nothing to join: the case is IGNORED,
// not failed, and says so. The runner cannot take the access point away, so
// cut and restore are NULL and the checks that need them are reported as not
// exercised; the board's own recovery checks are the open item "Wi-Fi on the
// board" (docs/hardware/board_notes.md). Time is the board's: the run takes
// about two minutes, most of it the contract's waits while the link is down.
//
// The contract runs at the firmware's caller's priority, the event loop's,
// above the station's task. The test task's own, 1, is below it on the same
// core: the first up() woke the station's task, which preempted the caller
// and started the radio -- its RF calibration, 39 ms of CPU -- before up()
// could return, and "up() returns without waiting" failed (2026-10-21) on
// time up() had not spent waiting. In the firmware the caller outranks the
// station's task and runs on the other core.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "sdkconfig.h"  // CONFIG_HAC_WIFI_*, from sdkconfig.credentials

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "unity.h"

#include "adp_clock.h"
#include "adp_log.h"
#include "adp_net.h"
#include "adp_net_pure.h"
#include "contract_print.h"
#include "port_net_contract.h"

// The firmware's, as its main.c gives them.
#define NET_TASK_STACK_BYTES 6144u
#define NET_TASK_PRIORITY 4
#define NET_TASK_CORE 0

// The firmware's caller of up() and down(): the event loop
// (EVENT_LOOP_TASK_PRIORITY in its main.c), above NET_TASK_PRIORITY.
#define CALLER_PRIORITY 5

#define IMPLEMENTATION "adp_net+ESP-IDF station"

static adp_log_t s_log;
static adp_clock_t s_clock;
static adp_net_t s_net;

static void report(void *ctx, const char *check, bool ok)
{
    contract_print_check((const contract_print_ctx_t *)ctx, check, ok);
}

// Real time passes: the station's task and ESP-IDF's run meanwhile.
static void advance(void *ctx, uint64_t ms)
{
    (void)ctx;
    const port_clock_t *clock = adp_clock_port(&s_clock);
    clock->delay_ms(clock, (uint32_t)ms);
}

TEST_CASE("adp_net satisfies the port_net contract", "[contract][port_net]")
{
    if (CONFIG_HAC_WIFI_SSID[0] == '\0') {
        printf("contract port_net %s: no network in this image -- set CONFIG_HAC_WIFI_SSID in "
               "apps/firmware_esp32/sdkconfig.credentials and rebuild from a fresh sdkconfig\n",
               IMPLEMENTATION);
        TEST_IGNORE_MESSAGE(
            "no Wi-Fi credentials in this image: the port_net contract was not run");
    }

    adp_log_init(&s_log);
    adp_clock_init(&s_clock, adp_log_port(&s_log));
    const port_clock_t *clock = adp_clock_port(&s_clock);
    const adp_net_config_t cfg = {
        .ssid = CONFIG_HAC_WIFI_SSID,
        .password = CONFIG_HAC_WIFI_PASSWORD,
        .hostname = NULL,
        .clock = clock,
        .log = adp_log_port(&s_log),
        .task_stack_bytes = NET_TASK_STACK_BYTES,
        .task_priority = NET_TASK_PRIORITY,
        .task_core = NET_TASK_CORE,
    };
    TEST_ASSERT_EQUAL_INT(HAL_OK, adp_net_init(&s_net, &cfg));

    const port_net_contract_env_t env = {
        .clock = clock,
        .advance = advance,
        .cut = NULL,
        .restore = NULL,
        .ctx = NULL,
    };
    contract_print_ctx_t ctx = {.port = "port_net", .implementation = IMPLEMENTATION};
    const uint64_t start = clock->now_ms(clock);
    const UBaseType_t own_priority = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, CALLER_PRIORITY);
    const unsigned failures = port_net_contract_run(adp_net_port(&s_net), &env, report, &ctx);
    vTaskPrioritySet(NULL, own_priority);

    adp_net_status_t st;
    adp_net_status(&s_net, &st);
    printf("contract port_net %s: %lu s in all; %lu connections, %lu attempts, %lu failed "
           "(%lu timed out), the last reason %s (%u); task stack %lu bytes free at least, %lu "
           "events dropped\n",
           IMPLEMENTATION, (unsigned long)((clock->now_ms(clock) - start) / 1000u),
           (unsigned long)st.stats.connections, (unsigned long)st.stats.attempts,
           (unsigned long)st.stats.attempts_failed, (unsigned long)st.stats.timeouts,
           adp_net_reason_name(st.stats.last_reason), (unsigned)st.stats.last_reason,
           (unsigned long)st.stack_free_min_bytes, (unsigned long)st.events_dropped);
    TEST_ASSERT_EQUAL_UINT(0u, failures);
}
