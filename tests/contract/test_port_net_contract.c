// SPDX-License-Identifier: Apache-2.0
//
// Host runner for port_net_contract.c, the shared body, against the
// simulator's network (platform/host/adp_net_sim.h), whose access point the
// runner takes away and gives back. Then against nine deliberately broken
// wrappers over it, each of which the suite must catch.

#include "port_net_contract.h"

#include <stdio.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_net_sim.h"
#include "contract_print.h"
#include "hac_test.h"

static adp_clock_virtual_t g_clock;
static adp_net_sim_t g_net;

static void report_to_hac_test(void *ctx, const char *check, bool ok)
{
    contract_print_check((const contract_print_ctx_t *)ctx, check, ok);
    hac_test_check(ok, check, __FILE__, __LINE__);
}

static void advance(void *ctx, uint64_t ms)
{
    (void)ctx;
    adp_clock_virtual_advance(&g_clock, ms);
}

static void cut(void *ctx)
{
    (void)ctx;
    adp_net_sim_set_link(&g_net, false);
}

static void restore(void *ctx)
{
    (void)ctx;
    adp_net_sim_set_link(&g_net, true);
}

static port_net_contract_env_t env_with(bool can_cut)
{
    port_net_contract_env_t env;
    memset(&env, 0, sizeof(env));
    env.clock = adp_clock_virtual_port(&g_clock);
    env.advance = advance;
    env.cut = can_cut ? cut : NULL;
    env.restore = can_cut ? restore : NULL;
    return env;
}

static void fresh(void)
{
    adp_clock_virtual_init(&g_clock, 1.0);
    adp_net_sim_init(&g_net, adp_clock_virtual_port(&g_clock));
}

static void adp_net_sim_satisfies_the_port_net_contract(void)
{
    fresh();
    const port_net_contract_env_t env = env_with(true);
    contract_print_ctx_t ctx = {.port = "port_net", .implementation = "adp_net_sim"};
    HAC_CHECK_EQ_INT(
        (int)port_net_contract_run(adp_net_sim_port(&g_net), &env, report_to_hac_test, &ctx), 0);
    HAC_CHECK_EQ_INT((int)g_net.links_lost, 1);
}

// The checks that need the access point taken away say so, and pass, on a
// runner that cannot -- as the board's will.
static void a_runner_that_cannot_cut_the_link_is_told_so(void)
{
    fresh();
    const port_net_contract_env_t env = env_with(false);
    contract_print_ctx_t ctx = {.port = "port_net", .implementation = "adp_net_sim(no-cut)"};
    HAC_CHECK_EQ_INT(
        (int)port_net_contract_run(adp_net_sim_port(&g_net), &env, report_to_hac_test, &ctx), 0);
}

// --- broken wrappers ------------------------------------------------------------------

typedef enum {
    MUTANT_BLOCKING_UP = 0,  // up() waits for the association
    MUTANT_RSSI_ALWAYS,      // a signal reported while down
    MUTANT_STALE_UP,         // once up, says up for good
    MUTANT_NO_RECONNECT,     // a lost link stays lost until up() again
    MUTANT_DOWN_IGNORED,     // down() does nothing
    MUTANT_UP_UNASKED,       // comes up without up()
    MUTANT_ADDRESS_ALWAYS,   // an address reported while down
    MUTANT_NO_ADDRESS,       // up, without an address
    MUTANT_NAME_WHILE_UP,    // the network's name only while up
} mutant_kind_t;

typedef struct {
    port_net_t port;
    port_net_t *inner;
    mutant_kind_t kind;
    bool seen_up;
    bool lost;
} mutant_t;

static net_state_t mutant_state(const port_net_t *port)
{
    mutant_t *m = (mutant_t *)port->impl;
    if (m->kind == MUTANT_UP_UNASKED) {
        (void)m->inner->up(m->inner);
    }
    const net_state_t s = m->inner->state(m->inner);
    if (m->kind == MUTANT_STALE_UP) {
        m->seen_up = m->seen_up || s == NET_UP;
        if (m->seen_up && s != NET_DOWN) {
            return NET_UP;
        }
    }
    if (m->kind == MUTANT_NO_RECONNECT) {
        if (m->seen_up && s != NET_UP && s != NET_DOWN) {
            m->lost = true;
        }
        m->seen_up = m->seen_up || s == NET_UP;
        if (m->lost && s == NET_UP) {
            return NET_CONNECTING;
        }
    }
    return s;
}

static int8_t mutant_rssi(const port_net_t *port)
{
    const mutant_t *m = (const mutant_t *)port->impl;
    if (m->kind == MUTANT_RSSI_ALWAYS) {
        return (int8_t)ADP_NET_SIM_DEFAULT_RSSI_DBM;
    }
    return (mutant_state(port) == NET_UP) ? m->inner->rssi_dbm(m->inner)
                                          : (int8_t)PORT_NET_RSSI_NONE;
}

static void mutant_info(const port_net_t *port, net_info_t *out)
{
    const mutant_t *m = (const mutant_t *)port->impl;
    m->inner->info(m->inner, out);
    const bool up = mutant_state(port) == NET_UP;
    if (m->kind == MUTANT_ADDRESS_ALWAYS) {
        out->ipv4[0] = 10u;
        out->ipv4[3] = 7u;
    } else if (m->kind == MUTANT_NO_ADDRESS) {
        memset(out->ipv4, 0, sizeof(out->ipv4));
    } else if (m->kind == MUTANT_NAME_WHILE_UP && !up) {
        out->ssid[0] = '\0';
    }
}

static hal_status_t mutant_up(port_net_t *port)
{
    mutant_t *m = (mutant_t *)port->impl;
    m->lost = false;
    m->seen_up = false;
    if (m->kind == MUTANT_BLOCKING_UP) {
        adp_clock_virtual_advance(&g_clock, ADP_NET_SIM_CONNECT_MS);
    }
    return m->inner->up(m->inner);
}

static hal_status_t mutant_down(port_net_t *port)
{
    mutant_t *m = (mutant_t *)port->impl;
    if (m->kind == MUTANT_DOWN_IGNORED) {
        return HAL_OK;
    }
    return m->inner->down(m->inner);
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
        {MUTANT_BLOCKING_UP, "blocking_up", "up() returns without waiting"},
        {MUTANT_RSSI_ALWAYS, "rssi_always", "not up: rssi_dbm() is PORT_NET_RSSI_NONE"},
        {MUTANT_STALE_UP, "stale_up", "a lost link: no longer NET_UP"},
        {MUTANT_NO_RECONNECT, "no_reconnect", "the link back: up again without being asked"},
        {MUTANT_DOWN_IGNORED, "down_ignored", "down(): NET_DOWN once it returns"},
        {MUTANT_UP_UNASKED, "up_unasked", "never asked: still NET_DOWN a while later"},
        {MUTANT_ADDRESS_ALWAYS, "address_always", "not up: info() gives no address, 0.0.0.0"},
        {MUTANT_NO_ADDRESS, "no_address", "up: info() gives an address"},
        {MUTANT_NAME_WHILE_UP, "name_while_up", "up: info()'s name is the one it gave while down"},
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fresh();
        mutant_t m = {.inner = adp_net_sim_port(&g_net), .kind = cases[i].kind};
        m.port.up = mutant_up;
        m.port.down = mutant_down;
        m.port.state = mutant_state;
        m.port.rssi_dbm = mutant_rssi;
        m.port.info = mutant_info;
        m.port.impl = &m;
        const port_net_contract_env_t env = env_with(true);
        mutant_watch_t w = {.must_fail = cases[i].must_fail};
        const unsigned failures = port_net_contract_run(&m.port, &env, watch_report, &w);
        printf("contract port_net mutant %s: %u checks failed, \"%s\" %s\n", cases[i].name,
               failures, cases[i].must_fail, w.failed ? "caught" : "NOT CAUGHT");
        hac_test_check(failures > 0u && w.failed, "the suite catches the mutant", cases[i].name,
                       __LINE__);
    }
}

// --- what info() says -----------------------------------------------------------------

static void the_simulated_link_has_a_name_and_an_address_while_up(void)
{
    fresh();
    port_net_t *net = adp_net_sim_port(&g_net);
    net_info_t info;
    net->info(net, &info);
    HAC_CHECK(strcmp(info.ssid, ADP_NET_SIM_DEFAULT_SSID) == 0);
    HAC_CHECK(!port_net_ipv4_set(info.ipv4));
    (void)net->up(net);
    adp_clock_virtual_advance(&g_clock, ADP_NET_SIM_CONNECT_MS);
    net->info(net, &info);
    HAC_CHECK(strcmp(info.ssid, ADP_NET_SIM_DEFAULT_SSID) == 0);
    HAC_CHECK(info.ipv4[0] == ADP_NET_SIM_ADDRESS_0 && info.ipv4[1] == ADP_NET_SIM_ADDRESS_1 &&
              info.ipv4[2] == ADP_NET_SIM_ADDRESS_2 && info.ipv4[3] == ADP_NET_SIM_ADDRESS_3);
}

// --- the script -------------------------------------------------------------------------

static void the_script_cuts_and_restores_the_link_at_its_times(void)
{
    fresh();
    char err[160];
    HAC_CHECK_EQ_INT(adp_net_sim_add_script(&g_net, "10:down;20.5:up", err, sizeof(err)), HAL_OK);
    port_net_t *net = adp_net_sim_port(&g_net);
    HAC_CHECK_EQ_INT(net->up(net), HAL_OK);
    HAC_CHECK_EQ_U64(adp_net_sim_next_ms(&g_net), ADP_NET_SIM_CONNECT_MS);
    adp_clock_virtual_advance(&g_clock, ADP_NET_SIM_CONNECT_MS);
    HAC_CHECK_EQ_INT(net->state(net), NET_UP);
    HAC_CHECK_EQ_U64(adp_net_sim_next_ms(&g_net), 10000u);
    adp_clock_virtual_advance(&g_clock, 10000u - ADP_NET_SIM_CONNECT_MS);
    HAC_CHECK_EQ_INT(net->state(net), NET_CONNECTING);
    HAC_CHECK_EQ_INT(net->rssi_dbm(net), PORT_NET_RSSI_NONE);
    HAC_CHECK_EQ_U64(adp_net_sim_next_ms(&g_net), 20500u);
    adp_clock_virtual_advance(&g_clock, 10500u);
    HAC_CHECK_EQ_INT(net->state(net), NET_CONNECTING);  // associating again
    HAC_CHECK_EQ_U64(adp_net_sim_next_ms(&g_net), 20500u + ADP_NET_SIM_CONNECT_MS);
    adp_clock_virtual_advance(&g_clock, ADP_NET_SIM_CONNECT_MS);
    HAC_CHECK_EQ_INT(net->state(net), NET_UP);
    HAC_CHECK_EQ_U64(adp_net_sim_next_ms(&g_net), ADP_NET_SIM_NO_EVENT);
}

// A change the loop slept through is applied at its own time.
static uint64_t g_heard_at[4];
static bool g_heard_present[4];
static unsigned g_heard;

static void listener(void *ctx, bool present, uint64_t at_ms)
{
    (void)ctx;
    if (g_heard < 4u) {
        g_heard_present[g_heard] = present;
        g_heard_at[g_heard] = at_ms;
    }
    g_heard++;
}

static void the_listener_hears_each_change_at_its_own_time(void)
{
    fresh();
    g_heard = 0u;
    adp_net_sim_set_listener(&g_net, listener, NULL);
    char err[160];
    HAC_CHECK_EQ_INT(adp_net_sim_add_script(&g_net, "5:down;7:up", err, sizeof(err)), HAL_OK);
    adp_clock_virtual_advance(&g_clock, 60000u);
    (void)g_net.port.state(&g_net.port);
    HAC_CHECK_EQ_INT((int)g_heard, 2);
    HAC_CHECK(!g_heard_present[0] && g_heard_at[0] == 5000u);
    HAC_CHECK(g_heard_present[1] && g_heard_at[1] == 7000u);
}

static void a_malformed_script_is_refused_whole(void)
{
    static const struct {
        const char *text;
        const char *says;
    } cases[] = {
        {"10:sideways", "a change is <seconds>:down or <seconds>:up, not \"10:sideways\""},
        {"x:down", "a change is <seconds>:down or <seconds>:up, not \"x:down\""},
        {"10:down;5:up", "the changes must come in time order"},
        {"", "no changes in \"\""},
        {"-1:down", "a change is <seconds>:down or <seconds>:up, not \"-1:down\""},
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fresh();
        char err[160] = "";
        HAC_CHECK_EQ_INT(adp_net_sim_add_script(&g_net, cases[i].text, err, sizeof(err)),
                         HAL_ERR_INVALID_ARG);
        hac_test_check(strcmp(err, cases[i].says) == 0, cases[i].text, __FILE__, __LINE__);
        HAC_CHECK_EQ_INT((int)g_net.count, 0);
    }
}

// The table holds a link that flaps for half an hour; one change more is
// refused, and refused whole.
static void a_long_script_fits_up_to_the_table(void)
{
    fresh();
    static char text[ADP_NET_SIM_MAX_EVENTS * 16u];
    size_t len = 0u;
    for (unsigned i = 0u; i < ADP_NET_SIM_MAX_EVENTS; ++i) {
        len += (size_t)snprintf(text + len, sizeof(text) - len, "%s%u:%s", (i > 0u) ? ";" : "",
                                10u + i * 2u, (i % 2u == 0u) ? "down" : "up");
    }
    char err[160] = "";
    HAC_CHECK_EQ_INT(adp_net_sim_add_script(&g_net, text, err, sizeof(err)), HAL_OK);
    HAC_CHECK_EQ_INT((int)g_net.count, (int)ADP_NET_SIM_MAX_EVENTS);
    HAC_CHECK_EQ_INT(adp_net_sim_add_script(&g_net, "99999:down", err, sizeof(err)),
                     HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT((int)g_net.count, (int)ADP_NET_SIM_MAX_EVENTS);
}

int main(void)
{
    HAC_RUN(adp_net_sim_satisfies_the_port_net_contract);
    HAC_RUN(a_runner_that_cannot_cut_the_link_is_told_so);
    HAC_RUN(the_suite_catches_each_broken_wrapper);
    HAC_RUN(the_simulated_link_has_a_name_and_an_address_while_up);
    HAC_RUN(the_script_cuts_and_restores_the_link_at_its_times);
    HAC_RUN(the_listener_hears_each_change_at_its_own_time);
    HAC_RUN(a_malformed_script_is_refused_whole);
    HAC_RUN(a_long_script_fits_up_to_the_table);
    return hac_test_summary();
}
