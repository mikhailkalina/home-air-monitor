// SPDX-License-Identifier: Apache-2.0
//
// Host runner for port_telemetry_contract.c, the shared body, against the
// loopback client of the in-memory broker
// (platform/host/adp_telemetry_loopback.h) -- its network taken away and
// given back, the broker stalled and released. Then against six deliberately
// broken wrappers over it, each of which the suite must catch. Then against
// the libmosquitto client (platform/host/adp_telemetry_mqtt.h) and a real
// broker: a private mosquitto this runner starts, or the one
// HAC_TEST_MQTT_BROKER names -- on real time, its network taken away and
// given back, the broker never stalled. A build without libmosquitto, or
// without a broker to run, says that case was not exercised.

// getenv() and getpid() for the real broker's case: POSIX, before the first
// system header (platform/host/host_time.c says why).
#define _POSIX_C_SOURCE 200809L

#include "port_telemetry_contract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_telemetry_loopback.h"
#include "contract_print.h"
#include "hac_test.h"

#if defined(HAC_SIM_MQTT)
#include <unistd.h>

#include "adp_telemetry_mqtt.h"
#include "host_time.h"
#include "mqtt_test_broker.h"
#endif

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

// --- libmosquitto and a real broker -------------------------------------------------

// The environment that names a broker of one's own for this case, and the
// user and password it takes there -- the simulator's password variable.
#define BROKER_ENV "HAC_TEST_MQTT_BROKER"
#define BROKER_USER_ENV "HAC_TEST_MQTT_USER"
#define BROKER_PASSWORD_ENV "HAC_MQTT_PASSWORD"

#define WHY_MAX 256u

#if defined(HAC_SIM_MQTT)

// A real broker lives in real time, and so does this case's clock.
static uint64_t g_real_origin_ms;

static uint64_t real_now_ms(const port_clock_t *self)
{
    (void)self;
    return host_monotonic_ms() - g_real_origin_ms;
}

static uint64_t real_wall_ms(const port_clock_t *self)
{
    (void)self;
    return 0u;
}

static void real_delay_ms(const port_clock_t *self, uint32_t ms)
{
    (void)self;
    host_sleep_ms(ms);
}

static void real_advance(void *ctx, uint64_t ms)
{
    (void)ctx;
    host_sleep_ms((uint32_t)ms);
}

static void mqtt_cut(void *ctx)
{
    adp_telemetry_mqtt_set_link((adp_telemetry_mqtt_t *)ctx, false);
}

static void mqtt_restore(void *ctx)
{
    adp_telemetry_mqtt_set_link((adp_telemetry_mqtt_t *)ctx, true);
}

static void the_libmosquitto_client_satisfies_the_contract_with_a_real_broker(void)
{
    adp_telemetry_mqtt_url_t where;
    memset(&where, 0, sizeof(where));
    mqtt_test_broker_t broker;
    memset(&broker, 0, sizeof(broker));
    char why[WHY_MAX];

    const char *url = getenv(BROKER_ENV);
    if (url != NULL && url[0] != '\0') {
        if (adp_telemetry_mqtt_parse_url(url, &where, why, sizeof(why)) != HAL_OK) {
            printf("%s: %s\n", BROKER_ENV, why);
            HAC_CHECK(false);
            return;
        }
    } else {
#if defined(HAC_MOSQUITTO_BROKER)
        // A broker found when the build was configured that does not run is
        // a broken machine, not a missing one: it fails.
        const bool started = mqtt_test_broker_start(&broker, HAC_MOSQUITTO_BROKER, HAC_TEST_TMP_DIR,
                                                    why, sizeof(why));
        if (!started) {
            printf("contract port_telemetry adp_telemetry_mqtt: cannot start %s: %s\n",
                   HAC_MOSQUITTO_BROKER, why);
        }
        HAC_CHECK(started);
        if (!started) {
            return;
        }
        (void)snprintf(where.host, sizeof(where.host), "127.0.0.1");
        where.port = broker.port;
#else
        printf("contract port_telemetry adp_telemetry_mqtt: not exercised: no mosquitto broker "
               "was found when this build was configured, and %s names none\n",
               BROKER_ENV);
        return;
#endif
    }

    const port_clock_t clock = {
        .now_ms = real_now_ms, .wall_ms = real_wall_ms, .delay_ms = real_delay_ms, .impl = NULL};
    g_real_origin_ms = host_monotonic_ms();
    const adp_telemetry_mqtt_config_t cfg = {
        .host = where.host,
        .port = where.port,
        .username = getenv(BROKER_USER_ENV),
        .password = (getenv(BROKER_USER_ENV) != NULL) ? getenv(BROKER_PASSWORD_ENV) : NULL,
        .log = NULL,
    };
    adp_telemetry_mqtt_t *client = adp_telemetry_mqtt_create(&cfg, why, sizeof(why));
    HAC_CHECK(client != NULL);
    if (client == NULL) {
        printf("adp_telemetry_mqtt_create: %s\n", why);
        mqtt_test_broker_stop(&broker);
        return;
    }

    // An id and a subtree of this process's own, so that runs side by side
    // -- the debug and the ASan builds' -- never take each other's sessions.
    char client_id[PORT_TELEMETRY_CLIENT_ID_MAX + 1u];
    char root[PORT_TELEMETRY_CONTRACT_ROOT_MAX + 1u];
    (void)snprintf(client_id, sizeof(client_id), "hac-contract-%ld", (long)getpid());
    (void)snprintf(root, sizeof(root), "hac-contract/mqtt-%ld", (long)getpid());

    port_telemetry_contract_env_t env;
    memset(&env, 0, sizeof(env));
    env.clock = &clock;
    env.advance = real_advance;
    env.client_id = client_id;
    env.topic_root = root;
    env.cut = mqtt_cut;
    env.restore = mqtt_restore;
    env.stall = NULL;  // a real broker takes what it is sent
    env.ctx = client;
    printf("contract port_telemetry adp_telemetry_mqtt: libmosquitto %s, broker %s:%u%s\n",
           adp_telemetry_mqtt_library_version(), where.host, (unsigned)where.port,
           (broker.pid != 0) ? " (private)" : "");
    contract_print_ctx_t ctx = {.port = "port_telemetry", .implementation = "adp_telemetry_mqtt"};
    HAC_CHECK_EQ_INT((int)port_telemetry_contract_run(adp_telemetry_mqtt_port(client), &env,
                                                      report_to_hac_test, &ctx),
                     0);

    const adp_telemetry_mqtt_stats_t st = adp_telemetry_mqtt_stats(client);
    printf("contract port_telemetry adp_telemetry_mqtt: %u attempts, %u sessions, %u severed, "
           "%u published, %u acknowledged, %u received\n",
           st.attempts, st.sessions, st.severed, st.published, st.acknowledged, st.received);
    adp_telemetry_mqtt_destroy(client);
    mqtt_test_broker_stop(&broker);
    if (broker.log_path[0] != '\0') {
        printf("contract port_telemetry adp_telemetry_mqtt: the broker's log is %s\n",
               broker.log_path);
    }
}

#else

static void the_libmosquitto_client_satisfies_the_contract_with_a_real_broker(void)
{
    printf("contract port_telemetry adp_telemetry_mqtt: not exercised: this build has no "
           "libmosquitto (platform/host/CMakeLists.txt)\n");
}

#endif

int main(void)
{
    HAC_RUN(the_loopback_client_satisfies_the_port_telemetry_contract);
    HAC_RUN(a_runner_without_the_network_or_the_broker_is_told_so);
    HAC_RUN(the_suite_catches_each_broken_wrapper);
    HAC_RUN(the_libmosquitto_client_satisfies_the_contract_with_a_real_broker);
    return hac_test_summary();
}
