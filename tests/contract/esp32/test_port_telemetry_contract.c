// SPDX-License-Identifier: Apache-2.0
//
// On-target runner for port_telemetry_contract.c: the same body the host
// runner (tests/contract/test_port_telemetry_contract.c) runs against the
// loopback broker model and libmosquitto, here against adp_mqtt -- esp-mqtt
// on the board, to the broker sdkconfig.credentials names, over the Wi-Fi
// station the port_net case shares (test_station.h) (phase 4g,
// docs/adr/0033-mqtt-on-the-board.md). Built into the test app with
//
//   cd apps/firmware_esp32/test_app && idf.py -T hac_platform build
//
// An image without a network and a broker has nothing to connect to: the case
// is IGNORED, not failed, and says so. The network can be taken away under a
// session -- the station's down(), then up() -- so the drop and its will are
// exercised; the broker cannot be made to stop taking messages, so `stall`
// is NULL and the full queue is reported as not exercised. Every topic is
// under hac-contract/board-<MAC>, cleared before the run ends, and the client
// id is contract-<MAC>, never the board's own, so the device's place in Home
// Assistant is not touched. Real time: about two minutes.
//
// The contract runs where the firmware's caller runs: on a task of the event
// loop's priority on core 1, away from the station's, the adapter's and
// esp-mqtt's tasks on core 0 (main.c), so that its timing checks time the
// calls and nothing that preempts them.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "sdkconfig.h"  // CONFIG_HAC_WIFI_*, CONFIG_HAC_MQTT_*, from sdkconfig.credentials

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "unity.h"

#include "adp_mqtt.h"
#include "adp_net.h"
#include "contract_print.h"
#include "port_telemetry_contract.h"
#include "test_station.h"

// The firmware's, as its main.c gives them.
#define MQTT_TASK_STACK_BYTES 6144u
#define MQTT_TASK_PRIORITY 4
#define MQTT_TASK_CORE 0

// The firmware's caller: the event loop (EVENT_LOOP_TASK_* in its main.c).
#define CALLER_STACK_BYTES 8192u
#define CALLER_PRIORITY 5
#define CALLER_CORE 1

// How long the link may take to come up: an attempt and its backoff
// (ADP_NET_ATTEMPT_TIMEOUT_MS) twice over.
#define LINK_UP_MS 60000u

// How long the whole contract may take before the case gives up waiting.
#define CONTRACT_MAX_MS 600000u

#define IMPLEMENTATION "adp_mqtt+esp-mqtt"

static const test_station_t *s_station;
static adp_mqtt_t s_mqtt;
static char s_client_id[PORT_TELEMETRY_CLIENT_ID_MAX + 1u];
static char s_root[PORT_TELEMETRY_CONTRACT_ROOT_MAX + 1u];
static port_telemetry_contract_env_t s_env;
static contract_print_ctx_t s_ctx = {.port = "port_telemetry", .implementation = IMPLEMENTATION};
static unsigned s_failures;
static SemaphoreHandle_t s_done;

static void report(void *ctx, const char *check, bool ok)
{
    contract_print_check((const contract_print_ctx_t *)ctx, check, ok);
}

// Real time passes: the station's task, the adapter's and esp-mqtt's run
// meanwhile.
static void advance(void *ctx, uint64_t ms)
{
    (void)ctx;
    s_station->clock->delay_ms(s_station->clock, (uint32_t)ms);
}

// The network under the session goes: the station taken down, as Wi-Fi going
// would leave it.
static void cut(void *ctx)
{
    (void)ctx;
    port_net_t *net = adp_net_port(s_station->net);
    (void)net->down(net);
}

// ... and comes back: the contract connects again at once, so this waits for
// the link.
static void restore(void *ctx)
{
    (void)ctx;
    if (!test_station_up(s_station, LINK_UP_MS)) {
        printf("contract port_telemetry %s: the link did not come back within %lu s\n",
               IMPLEMENTATION, (unsigned long)(LINK_UP_MS / 1000u));
    }
}

static void caller_task(void *arg)
{
    (void)arg;
    s_failures = port_telemetry_contract_run(adp_mqtt_port(&s_mqtt), &s_env, report, &s_ctx);
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

TEST_CASE("adp_mqtt satisfies the port_telemetry contract", "[contract][port_telemetry]")
{
    if (CONFIG_HAC_WIFI_SSID[0] == '\0' || CONFIG_HAC_MQTT_BROKER_URI[0] == '\0') {
        printf("contract port_telemetry %s: no network or no broker in this image -- set "
               "CONFIG_HAC_WIFI_SSID and CONFIG_HAC_MQTT_BROKER_URI in "
               "apps/firmware_esp32/sdkconfig.credentials and rebuild from a fresh sdkconfig\n",
               IMPLEMENTATION);
        TEST_IGNORE_MESSAGE("no broker in this image: the port_telemetry contract was not run");
    }

    s_station = test_station();
    TEST_ASSERT_EQUAL_INT(HAL_OK, s_station->status);
    TEST_ASSERT_TRUE_MESSAGE(test_station_up(s_station, LINK_UP_MS), "the link did not come up");

    uint8_t mac[ADP_NET_MAC_LEN];
    TEST_ASSERT_EQUAL_INT(HAL_OK, adp_net_station_mac(mac));
    (void)snprintf(s_client_id, sizeof(s_client_id), "contract-%02x%02x%02x%02x%02x%02x", mac[0],
                   mac[1], mac[2], mac[3], mac[4], mac[5]);
    (void)snprintf(s_root, sizeof(s_root), "hac-contract/board-%02x%02x%02x%02x%02x%02x", mac[0],
                   mac[1], mac[2], mac[3], mac[4], mac[5]);

    const adp_mqtt_config_t cfg = {
        .broker_uri = CONFIG_HAC_MQTT_BROKER_URI,
        .username = CONFIG_HAC_MQTT_USER,
        .password = CONFIG_HAC_MQTT_PASSWORD,
        .net = adp_net_port(s_station->net),
        .clock = s_station->clock,
        .log = s_station->log,
        .task_stack_bytes = MQTT_TASK_STACK_BYTES,
        .task_priority = MQTT_TASK_PRIORITY,
        .task_core = MQTT_TASK_CORE,
    };
    TEST_ASSERT_EQUAL_INT(HAL_OK, adp_mqtt_init(&s_mqtt, &cfg));

    s_env.clock = s_station->clock;
    s_env.advance = advance;
    s_env.client_id = s_client_id;
    s_env.topic_root = s_root;
    s_env.cut = cut;
    s_env.restore = restore;
    s_env.stall = NULL;
    s_env.ctx = NULL;

    adp_mqtt_status_t st;
    adp_mqtt_status(&s_mqtt, &st);
    printf("contract port_telemetry %s: broker %s:%u, %s, client id %s, topics under %s\n",
           IMPLEMENTATION, st.broker.host, (unsigned)st.broker.port,
           (st.username[0] != '\0') ? "with a user" : "anonymous", s_client_id, s_root);

    s_done = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(s_done);
    const uint64_t start = s_station->clock->now_ms(s_station->clock);
    TEST_ASSERT_EQUAL_INT(pdPASS,
                          xTaskCreatePinnedToCore(caller_task, "contract", CALLER_STACK_BYTES, NULL,
                                                  CALLER_PRIORITY, NULL, CALLER_CORE));
    const bool finished = xSemaphoreTake(s_done, pdMS_TO_TICKS(CONTRACT_MAX_MS)) == pdTRUE;

    adp_mqtt_status(&s_mqtt, &st);
    printf("contract port_telemetry %s: %lu s in all; %lu attempts, %lu sessions, %lu by the "
           "network; %lu published, %lu forwarded, %lu acknowledged, %lu received; the longest "
           "call %lu us (%s); stacks %lu bytes free at least (the adapter's), %lu (esp-mqtt's)\n",
           IMPLEMENTATION,
           (unsigned long)((s_station->clock->now_ms(s_station->clock) - start) / 1000u),
           (unsigned long)st.stats.attempts, (unsigned long)st.stats.sessions,
           (unsigned long)st.stats.network, (unsigned long)st.stats.published,
           (unsigned long)st.stats.forwarded, (unsigned long)st.stats.acknowledged,
           (unsigned long)st.stats.received, (unsigned long)st.post_max_us, st.post_max_call,
           (unsigned long)st.task_stack_free_min_bytes,
           (unsigned long)st.mqtt_stack_free_min_bytes);
    port_net_t *net = adp_net_port(s_station->net);
    (void)net->down(net);
    TEST_ASSERT_TRUE_MESSAGE(finished, "the contract did not finish in time");
    TEST_ASSERT_EQUAL_UINT(0u, s_failures);
}
