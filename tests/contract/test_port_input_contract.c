// SPDX-License-Identifier: Apache-2.0
//
// Host runner for port_input_contract.c, the shared body, against every
// port_input_t this build has: gt911_input -- the board's own code -- over
// the GT911 model on the fake bus; the simulator's scripted taps; and, where
// SDL is built, the window's mouse-to-touch adapter, fed synthetic mouse
// events with no window at all. Then against five deliberately broken
// wrappers over gt911_input, each of which the suite must catch.

#include "port_input_contract.h"

#include <stdio.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_input_script.h"
#include "contract_print.h"
#include "gt911_input.h"
#include "gt911_rig.h"
#include "hac_test.h"
#ifdef HAC_SIM_GUI
#include <SDL.h>

#include "adp_input_sdl.h"
#endif

// A GT911 scan cycle, which the runner's advance() steps through.
#define SCAN_MS 10u

static void report_to_hac_test(void *ctx, const char *check, bool ok)
{
    contract_print_check((const contract_print_ctx_t *)ctx, check, ok);
    hac_test_check(ok, check, __FILE__, __LINE__);
}

// --- gt911_input over the model -------------------------------------------------------

static gt911_rig_t g_rig;

// The panel to the model's portrait frame: the inverse of gt911_rig_map().
static gt911_model_point_t from_panel(uint16_t x, uint16_t y)
{
    return gt911_rig_point(0u, (uint16_t)(GT911_RIG_PANEL_H - 1u - y), x);
}

static void gt911_press(void *ctx, uint16_t x, uint16_t y)
{
    (void)ctx;
    const gt911_model_point_t p = from_panel(x, y);
    gt911_model_touch(&g_rig.model, &p, 1u);
}

static void gt911_lift(void *ctx)
{
    (void)ctx;
    gt911_model_touch(&g_rig.model, NULL, 0u);
}

// Time passing on the board: a scan every SCAN_MS, the task answering each
// interrupt, and the reads it schedules for itself.
static void gt911_advance(void *ctx, uint64_t ms)
{
    (void)ctx;
    for (uint64_t t = 0u; t < ms; t += SCAN_MS) {
        adp_clock_virtual_advance(&g_rig.clock, SCAN_MS);
        gt911_model_scan(&g_rig.model);
        (void)gt911_input_service(&g_rig.input);
    }
}

static port_input_contract_env_t gt911_env(void)
{
    port_input_contract_env_t env;
    memset(&env, 0, sizeof(env));
    env.panel_width = GT911_RIG_PANEL_W;
    env.panel_height = GT911_RIG_PANEL_H;
    env.clock = adp_clock_virtual_port(&g_rig.clock);
    env.advance = gt911_advance;
    env.press = gt911_press;
    env.drag = gt911_press;  // the same finger, now there
    env.lift = gt911_lift;
    return env;
}

static void gt911_rig_up(void)
{
    gt911_rig_init(&g_rig, GT911_RIG_ADDR);
    HAC_CHECK_EQ_INT(gt911_rig_input_setup(&g_rig), HAL_OK);
    HAC_CHECK_EQ_INT(gt911_input_init(&g_rig.input), HAL_OK);
}

static unsigned run(port_input_t *in, const port_input_contract_env_t *env, const char *impl)
{
    contract_print_ctx_t ctx = {.port = "port_input", .implementation = impl};
    return port_input_contract_run(in, env, report_to_hac_test, &ctx);
}

static void gt911_input_satisfies_the_port_input_contract(void)
{
    gt911_rig_up();
    const port_input_contract_env_t env = gt911_env();
    HAC_CHECK_EQ_INT((int)run(gt911_input_port(&g_rig.input), &env, "gt911_input+model"), 0);
    HAC_CHECK_EQ_INT((int)g_rig.model.stats.config_writes, 0);
}

// --- the scripted taps -------------------------------------------------------------------

static adp_clock_virtual_t g_clock;
static adp_input_script_t g_script;

static void script_press(void *ctx, uint16_t x, uint16_t y)
{
    (void)ctx;
    (void)adp_input_script_push(&g_script, INPUT_TOUCH_DOWN, x, y, g_clock.now_ms);
}

static void script_drag(void *ctx, uint16_t x, uint16_t y)
{
    (void)ctx;
    (void)adp_input_script_push(&g_script, INPUT_TOUCH_MOVE, x, y, g_clock.now_ms);
}

static void script_lift(void *ctx)
{
    (void)ctx;
    // Up where it was last: the script's last event's position.
    const input_event_t *last = &g_script.events[g_script.count - 1u];
    (void)adp_input_script_push(&g_script, INPUT_TOUCH_UP, last->x, last->y, g_clock.now_ms);
}

static void clock_advance(void *ctx, uint64_t ms)
{
    (void)ctx;
    adp_clock_virtual_advance(&g_clock, ms);
}

static void adp_input_script_satisfies_the_port_input_contract(void)
{
    adp_clock_virtual_init(&g_clock, 1.0);
    adp_input_script_init(&g_script, adp_clock_virtual_port(&g_clock));
    port_input_contract_env_t env;
    memset(&env, 0, sizeof(env));
    env.panel_width = GT911_RIG_PANEL_W;
    env.panel_height = GT911_RIG_PANEL_H;
    env.clock = adp_clock_virtual_port(&g_clock);
    env.advance = clock_advance;
    env.press = script_press;
    env.drag = script_drag;
    env.lift = script_lift;
    HAC_CHECK_EQ_INT((int)run(adp_input_script_port(&g_script), &env, "adp_input_script"), 0);
}

// --- the window's mouse, without a window ------------------------------------------------

#ifdef HAC_SIM_GUI
static int g_mouse_x;
static int g_mouse_y;

static void push_mouse(Uint32 type, int x, int y)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    if (type == SDL_MOUSEMOTION) {
        ev.motion.x = x;
        ev.motion.y = y;
    } else {
        ev.button.button = SDL_BUTTON_LEFT;
        ev.button.x = x;
        ev.button.y = y;
    }
    (void)SDL_PushEvent(&ev);
    g_mouse_x = x;
    g_mouse_y = y;
}

static void sdl_press(void *ctx, uint16_t x, uint16_t y)
{
    (void)ctx;
    push_mouse(SDL_MOUSEBUTTONDOWN, x, y);
}

static void sdl_drag(void *ctx, uint16_t x, uint16_t y)
{
    (void)ctx;
    push_mouse(SDL_MOUSEMOTION, x, y);
}

static void sdl_lift(void *ctx)
{
    (void)ctx;
    push_mouse(SDL_MOUSEBUTTONUP, g_mouse_x, g_mouse_y);
}

static void adp_input_sdl_satisfies_the_port_input_contract(void)
{
    // The event queue alone: no video, no window, no display needed.
    if (SDL_Init(SDL_INIT_EVENTS) != 0) {
        printf("contract port_input adp_input_sdl SDL_Init failed: %s -- not run\n",
               SDL_GetError());
        return;
    }
    adp_clock_virtual_init(&g_clock, 1.0);
    static adp_input_sdl_t sdl;
    adp_input_sdl_init(&sdl, GT911_RIG_PANEL_W, GT911_RIG_PANEL_H, 1.0f,
                       adp_clock_virtual_port(&g_clock));
    port_input_contract_env_t env;
    memset(&env, 0, sizeof(env));
    env.panel_width = GT911_RIG_PANEL_W;
    env.panel_height = GT911_RIG_PANEL_H;
    env.clock = adp_clock_virtual_port(&g_clock);
    env.advance = clock_advance;
    env.press = sdl_press;
    env.drag = sdl_drag;
    env.lift = sdl_lift;
    HAC_CHECK_EQ_INT((int)run(adp_input_sdl_port(&sdl), &env, "adp_input_sdl"), 0);
    SDL_Quit();
}
#endif

// --- broken wrappers ------------------------------------------------------------------------

typedef enum {
    MUTANT_PHANTOM = 0,  // an event while nothing touches the panel
    MUTANT_BLOCKING,     // poll() waits
    MUTANT_UNMAPPED,     // the axes left as the controller has them
    MUTANT_UP_OTHER_ID,  // the lift reported for another finger
    MUTANT_UP_DROPPED,   // the lift never reported
} mutant_kind_t;

typedef struct {
    port_input_t port;
    port_input_t *inner;
    mutant_kind_t kind;
    bool phantom_sent;
} mutant_t;

static bool mutant_poll(port_input_t *port, input_event_t *out)
{
    mutant_t *m = (mutant_t *)port->impl;
    if (m->kind == MUTANT_PHANTOM && !m->phantom_sent) {
        m->phantom_sent = true;
        memset(out, 0, sizeof(*out));
        out->kind = INPUT_TOUCH_DOWN;
        return true;
    }
    if (m->kind == MUTANT_BLOCKING) {
        adp_clock_virtual_advance(&g_rig.clock, 50u);
    }
    for (;;) {
        if (!m->inner->poll(m->inner, out)) {
            return false;
        }
        if (m->kind == MUTANT_UNMAPPED) {
            const uint16_t x = out->x;
            out->x = out->y;
            out->y = x;
        }
        if (out->kind == INPUT_TOUCH_UP && m->kind == MUTANT_UP_OTHER_ID) {
            out->id = (uint8_t)(out->id + 1u);
        }
        if (out->kind == INPUT_TOUCH_UP && m->kind == MUTANT_UP_DROPPED) {
            continue;
        }
        return true;
    }
}

typedef struct {
    const char *name;
    bool failed;  // the check this mutant must fail, did
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
        {MUTANT_PHANTOM, "phantom", "untouched: nothing to poll"},
        {MUTANT_BLOCKING, "blocking", "poll() never waits"},
        {MUTANT_UNMAPPED, "unmapped", "a finger down: where it landed"},
        {MUTANT_UP_OTHER_ID, "up_other_id", "a lift: one UP of the same finger"},
        {MUTANT_UP_DROPPED, "up_dropped", "a lift: one UP of the same finger"},
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        gt911_rig_up();
        mutant_t m = {.inner = gt911_input_port(&g_rig.input), .kind = cases[i].kind};
        m.port.poll = mutant_poll;
        m.port.impl = &m;
        const port_input_contract_env_t env = gt911_env();
        mutant_watch_t w = {.name = cases[i].name, .must_fail = cases[i].must_fail};
        const unsigned failures = port_input_contract_run(&m.port, &env, watch_report, &w);
        printf("contract port_input mutant %s: %u checks failed, \"%s\" %s\n", cases[i].name,
               failures, cases[i].must_fail, w.failed ? "caught" : "NOT CAUGHT");
        hac_test_check(failures > 0u && w.failed, "the suite catches the mutant", cases[i].name,
                       __LINE__);
    }
}

int main(void)
{
    HAC_RUN(gt911_input_satisfies_the_port_input_contract);
    HAC_RUN(adp_input_script_satisfies_the_port_input_contract);
#ifdef HAC_SIM_GUI
    HAC_RUN(adp_input_sdl_satisfies_the_port_input_contract);
#endif
    HAC_RUN(the_suite_catches_each_broken_wrapper);
    return hac_test_summary();
}
