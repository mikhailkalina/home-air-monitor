// SPDX-License-Identifier: Apache-2.0

#include "test_station.h"

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"  // CONFIG_HAC_WIFI_*, from sdkconfig.credentials

#include "adp_clock.h"
#include "adp_log.h"

// The firmware's, as its main.c gives them.
#define NET_TASK_STACK_BYTES 6144u
#define NET_TASK_PRIORITY 4
#define NET_TASK_CORE 0

// How often test_station_up() looks.
#define POLL_MS 100u

static adp_log_t s_log;
static adp_clock_t s_clock;
static adp_net_t s_net;
static test_station_t s_station;
static bool s_ready;

const test_station_t *test_station(void)
{
    if (s_ready) {
        return &s_station;
    }
    s_ready = true;
    adp_log_init(&s_log);
    adp_clock_init(&s_clock, adp_log_port(&s_log));
    s_station.clock = adp_clock_port(&s_clock);
    s_station.log = adp_log_port(&s_log);
    s_station.net = &s_net;
    const adp_net_config_t cfg = {
        .ssid = CONFIG_HAC_WIFI_SSID,
        .password = CONFIG_HAC_WIFI_PASSWORD,
        .hostname = NULL,
        .clock = s_station.clock,
        .log = s_station.log,
        .task_stack_bytes = NET_TASK_STACK_BYTES,
        .task_priority = NET_TASK_PRIORITY,
        .task_core = NET_TASK_CORE,
    };
    s_station.status = adp_net_init(&s_net, &cfg);
    return &s_station;
}

bool test_station_up(const test_station_t *st, uint32_t within_ms)
{
    port_net_t *net = adp_net_port(st->net);
    if (net->up(net) != HAL_OK) {
        return false;
    }
    for (uint32_t waited = 0u;; waited += POLL_MS) {
        if (net->state(net) == NET_UP) {
            return true;
        }
        if (waited >= within_ms) {
            return false;
        }
        st->clock->delay_ms(st->clock, POLL_MS);
    }
}
