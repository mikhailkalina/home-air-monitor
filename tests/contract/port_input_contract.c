// SPDX-License-Identifier: Apache-2.0

#include "port_input_contract.h"

#include <stdbool.h>
#include <stddef.h>

// How long the runner lets pass after touching the panel before it polls.
#define SETTLE_MS 100u

// How long an untouched panel is watched for events that should not come.
#define QUIET_MS 500u

// Polls timed for the non-blocking check.
#define TIMED_POLLS 16u

// Events one step may produce: a drag across half the panel reports a move
// every few pixels.
#define MAX_EVENTS 64u

static void check(contract_report_fn report, void *ctx, unsigned *failures, const char *name,
                  bool ok)
{
    report(ctx, name, ok);
    if (!ok) {
        (*failures)++;
    }
}

static size_t drain(port_input_t *in, input_event_t *out, size_t cap)
{
    size_t n = 0u;
    input_event_t ev;
    // Bounded: an implementation that never runs dry fails the counts below
    // rather than hanging the runner.
    for (size_t guard = 0u; guard < 4u * MAX_EVENTS && in->poll(in, &ev); ++guard) {
        if (n < cap) {
            out[n] = ev;
        }
        ++n;
    }
    return n;
}

static bool near(uint16_t a, uint16_t b)
{
    const uint16_t d = (uint16_t)((a > b) ? a - b : b - a);
    return d <= PORT_INPUT_CONTRACT_POSITION_TOLERANCE_PX;
}

static bool inside(const port_input_contract_env_t *env, const input_event_t *ev)
{
    return ev->x < env->panel_width && ev->y < env->panel_height;
}

static uint64_t now(const port_input_contract_env_t *env)
{
    return env->clock->now_ms(env->clock);
}

static void not_exercised(contract_report_fn report, void *ctx, unsigned *failures)
{
    static const char *const checks[] = {
        "a finger down: one DOWN where it landed: not exercised by this runner",
        "a drag: MOVEs of the same finger: not exercised by this runner",
        "a lift: one UP of the same finger: not exercised by this runner",
        "events kept in order until polled: not exercised by this runner",
        "the far corner stays inside the panel: not exercised by this runner",
    };
    for (size_t i = 0u; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        check(report, ctx, failures, checks[i], true);
    }
}

unsigned port_input_contract_run(port_input_t *in, const port_input_contract_env_t *env,
                                 contract_report_fn report, void *report_ctx)
{
    unsigned failures = 0u;
    input_event_t ev[MAX_EVENTS];
    size_t n;

    // --- untouched --------------------------------------------------------------
    n = drain(in, ev, MAX_EVENTS);
    check(report, report_ctx, &failures, "untouched: nothing to poll", n == 0u);

    const uint64_t before = now(env);
    bool any = false;
    for (unsigned i = 0u; i < TIMED_POLLS; ++i) {
        input_event_t one;
        any = in->poll(in, &one) || any;
    }
    const uint64_t polls_took = now(env) - before;
    check(report, report_ctx, &failures, "poll() never waits",
          polls_took <= (uint64_t)PORT_INPUT_CONTRACT_MAX_POLL_MS);
    check(report, report_ctx, &failures, "untouched: poll() says false", !any);

    env->advance(env->ctx, QUIET_MS);
    n = drain(in, ev, MAX_EVENTS);
    check(report, report_ctx, &failures, "untouched for a while: still nothing", n == 0u);

    if (env->press == NULL || env->drag == NULL || env->lift == NULL) {
        not_exercised(report, report_ctx, &failures);
        return failures;
    }

    // --- a finger down, dragged, lifted --------------------------------------------
    const uint16_t x0 = (uint16_t)(env->panel_width / 3u);
    const uint16_t y0 = (uint16_t)(env->panel_height / 3u);
    const uint16_t x1 = (uint16_t)(2u * env->panel_width / 3u);
    const uint16_t y1 = (uint16_t)(2u * env->panel_height / 3u);

    const uint64_t pressed_at = now(env);
    env->press(env->ctx, x0, y0);
    env->advance(env->ctx, SETTLE_MS);
    n = drain(in, ev, MAX_EVENTS);
    const uint64_t polled_at = now(env);
    const bool one_down = n == 1u && ev[0].kind == INPUT_TOUCH_DOWN;
    check(report, report_ctx, &failures, "a finger down: one DOWN", one_down);
    check(report, report_ctx, &failures, "a finger down: where it landed",
          one_down && near(ev[0].x, x0) && near(ev[0].y, y0));
    check(report, report_ctx, &failures, "a finger down: stamped between the touch and the poll",
          one_down && ev[0].ts_ms >= pressed_at && ev[0].ts_ms <= polled_at);
    check(report, report_ctx, &failures, "a lone finger is id 0", one_down && ev[0].id == 0u);
    const uint8_t id = one_down ? ev[0].id : 0u;
    uint64_t last_ts = one_down ? ev[0].ts_ms : 0u;

    env->drag(env->ctx, x1, y1);
    env->advance(env->ctx, SETTLE_MS);
    n = drain(in, ev, MAX_EVENTS);
    bool moves_ok = n >= 1u && n <= MAX_EVENTS;
    bool in_order = true;
    for (size_t i = 0u; i < n && i < MAX_EVENTS; ++i) {
        moves_ok =
            moves_ok && ev[i].kind == INPUT_TOUCH_MOVE && ev[i].id == id && inside(env, &ev[i]);
        in_order = in_order && ev[i].ts_ms >= last_ts;
        last_ts = ev[i].ts_ms;
    }
    check(report, report_ctx, &failures, "a drag: MOVEs of the same finger, inside the panel",
          moves_ok);
    check(report, report_ctx, &failures, "a drag: the last move is where the finger is",
          moves_ok && near(ev[n - 1u].x, x1) && near(ev[n - 1u].y, y1));
    check(report, report_ctx, &failures, "a drag: stamped in order", in_order);

    env->lift(env->ctx);
    env->advance(env->ctx, SETTLE_MS);
    n = drain(in, ev, MAX_EVENTS);
    const bool one_up = n == 1u && ev[0].kind == INPUT_TOUCH_UP && ev[0].id == id;
    check(report, report_ctx, &failures, "a lift: one UP of the same finger", one_up);
    check(report, report_ctx, &failures, "a lift: stamped after the moves",
          one_up && ev[0].ts_ms >= last_ts);

    // --- kept until polled ------------------------------------------------------------
    env->press(env->ctx, x1, y0);
    env->advance(env->ctx, SETTLE_MS);
    env->lift(env->ctx);
    env->advance(env->ctx, SETTLE_MS);
    n = drain(in, ev, MAX_EVENTS);
    check(report, report_ctx, &failures, "events kept in order until polled",
          n == 2u && ev[0].kind == INPUT_TOUCH_DOWN && ev[1].kind == INPUT_TOUCH_UP &&
              ev[0].id == ev[1].id && ev[1].ts_ms >= ev[0].ts_ms && near(ev[0].x, x1) &&
              near(ev[0].y, y0));

    // --- the far corner -----------------------------------------------------------------
    const uint16_t xc = (uint16_t)(env->panel_width - 1u);
    const uint16_t yc = (uint16_t)(env->panel_height - 1u);
    env->press(env->ctx, xc, yc);
    env->advance(env->ctx, SETTLE_MS);
    env->lift(env->ctx);
    env->advance(env->ctx, SETTLE_MS);
    n = drain(in, ev, MAX_EVENTS);
    check(report, report_ctx, &failures, "the far corner stays inside the panel",
          n == 2u && inside(env, &ev[0]) && inside(env, &ev[1]) && near(ev[0].x, xc) &&
              near(ev[0].y, yc));

    env->advance(env->ctx, QUIET_MS);
    n = drain(in, ev, MAX_EVENTS);
    check(report, report_ctx, &failures, "nothing left over", n == 0u);
    return failures;
}
