// SPDX-License-Identifier: Apache-2.0
//
// Host runner for port_telemetry_contract.c, the shared body, against the
// loopback client of the in-memory broker
// (platform/host/adp_telemetry_loopback.h) -- its network taken away and
// given back, the broker stalled and released. Then against six deliberately
// broken wrappers over it, each of which the suite must catch.

#include "port_telemetry_contract.h"

#include <stdio.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_telemetry_loopback.h"
#include "contract_print.h"
#include "hac_test.h"

static adp_clock_virtual_t g_clock;
static loopback_broker_t g_broker;
static adp_telemetry_loopback_t g_client;

static void report_to_hac_test(void *ctx, const char *check, bool ok)
{
    contract_print_check((const contract_print_ctx_t *)ctx, check, ok);
    hac_test_check(ok, check, __FILE__, __LINE__);
}

static void advance(void *ctx, uint64_t ms)
{
    (void)ctx;
    adp_clock_virtual_advance(&g_clock, ms);
    loopback_broker_service(&g_broker);
}

static void cut(void *ctx)
{
    (void)ctx;
    adp_telemetry_loopback_set_link(&g_client, false, g_clock.now_ms);
}

static void restore(void *ctx)
{
    (void)ctx;
    adp_telemetry_loopback_set_link(&g_client, true, g_clock.now_ms);
}

static void stall(void *ctx, bool on)
{
    (void)ctx;
    loopback_broker_stall(&g_broker, on);
}

static port_telemetry_contract_env_t env_with(bool full)
{
    port_telemetry_contract_env_t env;
    memset(&env, 0, sizeof(env));
    env.clock = adp_clock_virtual_port(&g_clock);
    env.advance = advance;
    env.client_id = "contract-client";
    env.topic_root = "hac-contract/loopback";
    env.cut = full ? cut : NULL;
    env.restore = full ? restore : NULL;
    env.stall = full ? stall : NULL;
    return env;
}

static void fresh(void)
{
    adp_clock_virtual_init(&g_clock, 1.0);
    loopback_broker_init(&g_broker, adp_clock_virtual_port(&g_clock));
    HAC_CHECK_EQ_INT(adp_telemetry_loopback_init(&g_client, &g_broker), HAL_OK);
}

static void the_loopback_client_satisfies_the_port_telemetry_contract(void)
{
    fresh();
    const port_telemetry_contract_env_t env = env_with(true);
    contract_print_ctx_t ctx = {.port = "port_telemetry",
                                .implementation = "adp_telemetry_loopback"};
    HAC_CHECK_EQ_INT((int)port_telemetry_contract_run(adp_telemetry_loopback_port(&g_client), &env,
                                                      report_to_hac_test, &ctx),
                     0);
    // It left the broker as it found it, and the will went out once.
    HAC_CHECK_EQ_INT((int)g_broker.retained_count, 0);
    HAC_CHECK_EQ_INT((int)g_broker.wills_published, 1);
}

// The checks that need the network or the broker in the runner's hands say
// so, and pass, on a runner without them -- as the board's will.
static void a_runner_without_the_network_or_the_broker_is_told_so(void)
{
    fresh();
    const port_telemetry_contract_env_t env = env_with(false);
    contract_print_ctx_t ctx = {.port = "port_telemetry",
                                .implementation = "adp_telemetry_loopback(no-cut,no-stall)"};
    HAC_CHECK_EQ_INT((int)port_telemetry_contract_run(adp_telemetry_loopback_port(&g_client), &env,
                                                      report_to_hac_test, &ctx),
                     0);
}

// --- broken wrappers ------------------------------------------------------------------

typedef enum {
    MUTANT_DEFERRED = 0,      // keeps the caller's pointer, sends it later
    MUTANT_OFFLINE_OK,        // says HAL_OK without a session
    MUTANT_STALE_CONNECTED,   // once connected, says so for good
    MUTANT_RETAIN_LOST,       // never asks the broker to retain
    MUTANT_BLOCKING,          // publish() waits for the broker
    MUTANT_NO_MEM_SWALLOWED,  // a full queue reported as success
} mutant_kind_t;

typedef struct {
    port_telemetry_t port;
    port_telemetry_t *inner;
    mutant_kind_t kind;
    bool seen_connected;
    // MUTANT_DEFERRED's one message in hand.
    bool holding;
    const char *topic;
    const uint8_t *payload;
    size_t len;
    uint8_t qos;
    bool retain;
} mutant_t;

static mutant_t *mutant_of(const port_telemetry_t *port)
{
    return (mutant_t *)port->impl;
}

static void flush_held(mutant_t *m)
{
    if (m->holding) {
        m->holding = false;
        (void)m->inner->publish(m->inner, m->topic, m->payload, m->len, m->qos, m->retain);
    }
}

static hal_status_t mutant_connect(port_telemetry_t *port, const telemetry_session_t *s)
{
    mutant_t *m = mutant_of(port);
    flush_held(m);
    return m->inner->connect(m->inner, s);
}

static hal_status_t mutant_disconnect(port_telemetry_t *port)
{
    mutant_t *m = mutant_of(port);
    flush_held(m);
    return m->inner->disconnect(m->inner);
}

static hal_status_t mutant_publish(port_telemetry_t *port, const char *topic,
                                   const uint8_t *payload, size_t len, uint8_t qos, bool retain)
{
    mutant_t *m = mutant_of(port);
    flush_held(m);
    if (m->kind == MUTANT_RETAIN_LOST) {
        retain = false;
    }
    if (m->kind == MUTANT_BLOCKING) {
        adp_clock_virtual_advance(&g_clock, 50u);
    }
    if (m->kind == MUTANT_DEFERRED && m->inner->is_connected(m->inner) && topic != NULL &&
        port_telemetry_topic_ok(topic) && len < PORT_TELEMETRY_PAYLOAD_MAX && qos <= 2u &&
        (payload != NULL || len == 0u)) {
        m->holding = true;
        m->topic = topic;
        m->payload = payload;
        m->len = len;
        m->qos = qos;
        m->retain = retain;
        return HAL_OK;
    }
    const hal_status_t st = m->inner->publish(m->inner, topic, payload, len, qos, retain);
    if (st == HAL_ERR_NOT_READY && m->kind == MUTANT_OFFLINE_OK) {
        return HAL_OK;
    }
    if (st == HAL_ERR_NO_MEM && m->kind == MUTANT_NO_MEM_SWALLOWED) {
        return HAL_OK;
    }
    return st;
}

static hal_status_t mutant_subscribe(port_telemetry_t *port, const char *filter, uint8_t qos)
{
    mutant_t *m = mutant_of(port);
    flush_held(m);
    return m->inner->subscribe(m->inner, filter, qos);
}

static bool mutant_is_connected(const port_telemetry_t *port)
{
    mutant_t *m = mutant_of(port);
    const bool c = m->inner->is_connected(m->inner);
    m->seen_connected = m->seen_connected || c;
    return (m->kind == MUTANT_STALE_CONNECTED) ? m->seen_connected : c;
}

static bool mutant_poll(port_telemetry_t *port, telemetry_message_t *out)
{
    mutant_t *m = mutant_of(port);
    flush_held(m);
    return m->inner->poll(m->inner, out);
}

typedef struct {
    bool failed;
    const char *must_fail;
} mutant_watch_t;

static void watch_report(void *ctx, const char *check, bool ok)
{
    mutant_watch_t *w = (mutant_watch_t *)ctx;
    if (!ok && strcmp(check, w->must_fail) == 0) {
        w->failed = true;
    }
}

static void the_suite_catches_each_broken_wrapper(void)
{
    static const struct {
        mutant_kind_t kind;
        const char *name;
        const char *must_fail;
    } cases[] = {
        {MUTANT_DEFERRED, "deferred", "publish() copies the payload before it returns"},
        {MUTANT_OFFLINE_OK, "offline_ok", "before connect: publish() is HAL_ERR_NOT_READY"},
        {MUTANT_STALE_CONNECTED, "stale_connected", "a dropped session: not connected"},
        {MUTANT_RETAIN_LOST, "retain_lost",
         "a retained message reaches a new subscription, marked retained"},
        {MUTANT_BLOCKING, "blocking", "publish() and poll() never wait"},
        {MUTANT_NO_MEM_SWALLOWED, "no_mem_swallowed", "a full queue: publish() is HAL_ERR_NO_MEM"},
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fresh();
        mutant_t m;
        memset(&m, 0, sizeof(m));
        m.inner = adp_telemetry_loopback_port(&g_client);
        m.kind = cases[i].kind;
        m.port.connect = mutant_connect;
        m.port.disconnect = mutant_disconnect;
        m.port.publish = mutant_publish;
        m.port.subscribe = mutant_subscribe;
        m.port.is_connected = mutant_is_connected;
        m.port.poll = mutant_poll;
        m.port.impl = &m;
        const port_telemetry_contract_env_t env = env_with(true);
        mutant_watch_t w = {.must_fail = cases[i].must_fail};
        const unsigned failures = port_telemetry_contract_run(&m.port, &env, watch_report, &w);
        printf("contract port_telemetry mutant %s: %u checks failed, \"%s\" %s\n", cases[i].name,
               failures, cases[i].must_fail, w.failed ? "caught" : "NOT CAUGHT");
        hac_test_check(failures > 0u && w.failed, "the suite catches the mutant", cases[i].name,
                       __LINE__);
    }
}

int main(void)
{
    HAC_RUN(the_loopback_client_satisfies_the_port_telemetry_contract);
    HAC_RUN(a_runner_without_the_network_or_the_broker_is_told_so);
    HAC_RUN(the_suite_catches_each_broken_wrapper);
    return hac_test_summary();
}
