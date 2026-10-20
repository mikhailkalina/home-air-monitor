// SPDX-License-Identifier: Apache-2.0

#include "port_net_contract.h"

#include <stdbool.h>
#include <stddef.h>

// The step in which the runner lets time pass while it waits for a state.
#define STEP_MS 100u

// Calls timed for the non-blocking checks.
#define TIMED_CALLS 16u

static void check(contract_report_fn report, void *ctx, unsigned *failures, const char *name,
                  bool ok)
{
    report(ctx, name, ok);
    if (!ok) {
        (*failures)++;
    }
}

static uint64_t now(const port_net_contract_env_t *env)
{
    return env->clock->now_ms(env->clock);
}

static bool rssi_is_signal(int8_t rssi)
{
    return rssi >= -127 && rssi <= -1;
}

// Lets time pass until state() is `want`, at most `within_ms`; true when it
// got there. `others_ok` is cleared if a state outside `allowed` (a bit per
// net_state_t) is seen on the way.
static bool wait_for(port_net_t *net, const port_net_contract_env_t *env, net_state_t want,
                     uint64_t within_ms, unsigned allowed, bool *others_ok)
{
    for (uint64_t waited = 0u;; waited += STEP_MS) {
        const net_state_t s = net->state(net);
        if (s == want) {
            return true;
        }
        if (others_ok != NULL && (allowed & (1u << (unsigned)s)) == 0u) {
            *others_ok = false;
        }
        if (waited >= within_ms) {
            return false;
        }
        env->advance(env->ctx, STEP_MS);
    }
}

static void not_exercised(contract_report_fn report, void *ctx, unsigned *failures)
{
    static const char *const checks[] = {
        "a lost link: no longer NET_UP: not exercised by this runner",
        "a lost link: not NET_DOWN while up() is in force: not exercised by this runner",
        "a lost link: rssi_dbm() is PORT_NET_RSSI_NONE: not exercised by this runner",
        "the link back: up again without being asked: not exercised by this runner",
    };
    for (size_t i = 0u; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        check(report, ctx, failures, checks[i], true);
    }
}

unsigned port_net_contract_run(port_net_t *net, const port_net_contract_env_t *env,
                               contract_report_fn report, void *report_ctx)
{
    unsigned failures = 0u;
    const unsigned connecting_or_up = (1u << NET_CONNECTING) | (1u << NET_UP);

    // --- never asked ----------------------------------------------------------------
    check(report, report_ctx, &failures, "before up(): NET_DOWN", net->state(net) == NET_DOWN);
    check(report, report_ctx, &failures, "not up: rssi_dbm() is PORT_NET_RSSI_NONE",
          net->rssi_dbm(net) == PORT_NET_RSSI_NONE);

    uint64_t before = now(env);
    for (unsigned i = 0u; i < TIMED_CALLS; ++i) {
        (void)net->state(net);
        (void)net->rssi_dbm(net);
    }
    check(report, report_ctx, &failures, "state() and rssi_dbm() never wait",
          now(env) - before <= (uint64_t)PORT_NET_CONTRACT_MAX_CALL_MS);

    env->advance(env->ctx, PORT_NET_CONTRACT_UP_MS);
    check(report, report_ctx, &failures, "never asked: still NET_DOWN a while later",
          net->state(net) == NET_DOWN);

    // --- up --------------------------------------------------------------------------
    before = now(env);
    const hal_status_t up = net->up(net);
    check(report, report_ctx, &failures, "up() returns without waiting",
          now(env) - before <= (uint64_t)PORT_NET_CONTRACT_MAX_CALL_MS);
    check(report, report_ctx, &failures, "up() is HAL_OK", up == HAL_OK);

    bool on_the_way = true;
    const bool came_up =
        wait_for(net, env, NET_UP, PORT_NET_CONTRACT_UP_MS, connecting_or_up, &on_the_way);
    check(report, report_ctx, &failures, "after up(): connecting or up, never down or failed",
          on_the_way);
    check(report, report_ctx, &failures, "after up(): NET_UP within PORT_NET_CONTRACT_UP_MS",
          came_up);
    check(report, report_ctx, &failures, "up: rssi_dbm() is a signal, -127..-1 dBm",
          came_up && rssi_is_signal(net->rssi_dbm(net)));

    check(report, report_ctx, &failures, "up() again while up: HAL_OK", net->up(net) == HAL_OK);
    env->advance(env->ctx, PORT_NET_CONTRACT_NOTICE_MS);
    check(report, report_ctx, &failures, "up() again while up: still NET_UP",
          net->state(net) == NET_UP);

    // --- the access point goes, and comes back ---------------------------------------
    if (env->cut == NULL || env->restore == NULL) {
        not_exercised(report, report_ctx, &failures);
    } else {
        env->cut(env->ctx);
        bool not_down = true;
        const bool lost = wait_for(net, env, NET_CONNECTING, PORT_NET_CONTRACT_NOTICE_MS,
                                   (1u << NET_UP) | (1u << NET_FAILED), &not_down);
        const net_state_t s = net->state(net);
        check(report, report_ctx, &failures, "a lost link: no longer NET_UP",
              lost || s == NET_FAILED);
        check(report, report_ctx, &failures, "a lost link: not NET_DOWN while up() is in force",
              not_down && s != NET_DOWN);
        check(report, report_ctx, &failures, "a lost link: rssi_dbm() is PORT_NET_RSSI_NONE",
              s == NET_UP || net->rssi_dbm(net) == PORT_NET_RSSI_NONE);

        env->advance(env->ctx, PORT_NET_CONTRACT_NOTICE_MS);
        env->restore(env->ctx);
        check(report, report_ctx, &failures, "the link back: up again without being asked",
              wait_for(net, env, NET_UP, PORT_NET_CONTRACT_UP_MS, 0u, NULL));
    }

    // --- down ------------------------------------------------------------------------
    before = now(env);
    const hal_status_t down = net->down(net);
    check(report, report_ctx, &failures, "down() returns without waiting",
          now(env) - before <= (uint64_t)PORT_NET_CONTRACT_MAX_CALL_MS);
    check(report, report_ctx, &failures, "down() is HAL_OK", down == HAL_OK);
    check(report, report_ctx, &failures, "down(): NET_DOWN once it returns",
          net->state(net) == NET_DOWN);
    check(report, report_ctx, &failures, "down: rssi_dbm() is PORT_NET_RSSI_NONE",
          net->rssi_dbm(net) == PORT_NET_RSSI_NONE);
    check(report, report_ctx, &failures, "down() again: HAL_OK", net->down(net) == HAL_OK);
    env->advance(env->ctx, PORT_NET_CONTRACT_UP_MS);
    check(report, report_ctx, &failures, "down, the link there: still NET_DOWN a while later",
          net->state(net) == NET_DOWN);

    // --- up again ---------------------------------------------------------------------
    check(report, report_ctx, &failures, "up() after down(): HAL_OK", net->up(net) == HAL_OK);
    check(report, report_ctx, &failures, "up() after down(): NET_UP again",
          wait_for(net, env, NET_UP, PORT_NET_CONTRACT_UP_MS, 0u, NULL));

    (void)net->down(net);
    return failures;
}
