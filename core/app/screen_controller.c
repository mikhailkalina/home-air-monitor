// SPDX-License-Identifier: Apache-2.0

#include "app/screen_controller.h"

#include <string.h>

#include "ui/screen_diagnostics.h"
#include "ui/screen_home.h"
#include "ui/view_model.h"

screen_controller_config_t screen_controller_config_default(void)
{
    screen_controller_config_t cfg;
    cfg.tap = tap_config_default();
    cfg.diagnostics_timeout_ms = SCREEN_CONTROLLER_DEFAULT_DIAGNOSTICS_TIMEOUT_MS;
    return cfg;
}

void screen_controller_init(screen_controller_t *sc, const screen_controller_config_t *cfg)
{
    memset(sc, 0, sizeof(*sc));
    sc->cfg = *cfg;
    tap_init(&sc->tap, &cfg->tap);
    sc->screen = SCREEN_HOME;
}

static void switch_to(screen_controller_t *sc, update_policy_t *policy, screen_id_t screen,
                      bool by_tap, uint64_t asked_ms)
{
    sc->screen = screen;
    sc->change_pending = true;
    sc->change_by_tap = by_tap;
    sc->change_asked_ms = asked_ms;
    update_policy_request(policy);
}

bool screen_controller_input(screen_controller_t *sc, update_policy_t *policy,
                             const input_event_t *ev)
{
    if (ev->kind != INPUT_BUTTON) {
        sc->last_touch_ms = ev->ts_ms;
    }
    tap_event_t tap;
    if (!tap_feed(&sc->tap, ev, &tap)) {
        return false;
    }
    sc->taps++;
    const screen_id_t next = (sc->screen == SCREEN_HOME) ? SCREEN_DIAGNOSTICS : SCREEN_HOME;
    // Timed from the finger's lift, which is when the tap became one.
    switch_to(sc, policy, next, true, tap.up_ms);
    return true;
}

uint64_t screen_controller_tick(screen_controller_t *sc, update_policy_t *policy, uint64_t now_ms)
{
    if (sc->screen != SCREEN_DIAGNOSTICS) {
        return SCREEN_CONTROLLER_NO_DEADLINE;
    }
    // From the last touch, or from the tap that brought it if it is later --
    // the same one, unless the clocks disagree.
    uint64_t since = sc->last_touch_ms;
    if (sc->change_by_tap && sc->change_asked_ms > since) {
        since = sc->change_asked_ms;
    }
    const uint64_t home_at = since + sc->cfg.diagnostics_timeout_ms;
    if (now_ms < home_at) {
        return home_at;
    }
    sc->timeouts++;
    switch_to(sc, policy, SCREEN_HOME, false, now_ms);
    return SCREEN_CONTROLLER_NO_DEADLINE;
}

static diag_facts_t facts_of(const screen_inputs_t *in)
{
    diag_facts_t f;
    f.reading = in->reading;
    f.sensors = in->sensors;
    f.recorder = in->recorder;
    f.clock_source = in->clock_source;
    f.network = in->network;
    f.now_ms = in->now_ms;
    f.zone = in->zone;
    return f;
}

void screen_controller_evaluate(screen_controller_t *sc, update_policy_t *policy,
                                const screen_inputs_t *in, update_decision_t *out)
{
    if (sc->screen == SCREEN_HOME) {
        update_policy_evaluate(policy, in->reading, out);
        return;
    }
    const diag_facts_t facts = facts_of(in);
    diag_view_build(&sc->pending_diag, &facts);
    const bool changed =
        !sc->have_shown_diag || !diag_view_equal(&sc->pending_diag, &sc->shown_diag);
    // A developer's screen: content redraws stay partial without a count limit
    // (docs/adr/0038-the-diagnostics-screen-is-a-developers.md).
    update_policy_evaluate_partial_content(policy, changed, out);
}

void screen_controller_render(screen_controller_t *sc, framebuffer_t *fb, const screen_inputs_t *in)
{
    if (sc->screen == SCREEN_HOME) {
        view_model_t vm;
        view_model_build(&vm, in->reading, in->now_ms, in->zone);
        screen_home_render(fb, &vm);
        return;
    }
    const diag_facts_t facts = facts_of(in);
    diag_view_build(&sc->pending_diag, &facts);
    screen_diagnostics_render(fb, &sc->pending_diag);
}

void screen_controller_committed(screen_controller_t *sc, uint64_t now_ms,
                                 screen_redraw_note_t *note)
{
    memset(note, 0, sizeof(*note));
    note->screen = sc->screen;
    if (sc->screen == SCREEN_DIAGNOSTICS) {
        sc->shown_diag = sc->pending_diag;
        sc->have_shown_diag = true;
    } else {
        sc->have_shown_diag = false;  // the next visit draws its screen afresh
    }
    if (!sc->change_pending) {
        return;
    }
    sc->change_pending = false;
    note->screen_changed = true;
    note->by_tap = sc->change_by_tap;
    note->latency_ms = (now_ms >= sc->change_asked_ms) ? now_ms - sc->change_asked_ms : 0u;
}

screen_id_t screen_controller_screen(const screen_controller_t *sc)
{
    return sc->screen;
}

const char *screen_controller_screen_name(screen_id_t screen)
{
    switch (screen) {
        case SCREEN_HOME:
            return "home";
        case SCREEN_DIAGNOSTICS:
            return "diagnostics";
    }
    return "?";
}
