// SPDX-License-Identifier: Apache-2.0
//
// The screen controller (core/app/screen_controller.h) with the real update
// policy on the fake clock: a tap switches the screen and its redraw goes out
// at once, as a full refresh; the diagnostics screen redraws only
// when what it says changes, goes home after its timeout -- kept up by any
// touch -- and a redraw that carried a tap reports the latency from the
// finger's lift to the end of the flush.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "app/screen_controller.h"
#include "app/update_policy.h"
#include "fake_clock.h"
#include "hac_test.h"
#include "ui/framebuffer.h"

#define PANEL_W 960u
#define PANEL_H 540u

static fake_clock_t g_clock;
static update_policy_t g_policy;
static screen_controller_t g_sc;
static air_reading_t g_reading;
static sensor_manager_t g_mgr;
static sensor_source_t g_src = {.name = "scd4x", .id = 1u};
static time_zone_t g_zone;
static uint8_t g_pixels[PANEL_W / 2u * PANEL_H];
static framebuffer_t g_fb;

static screen_inputs_t inputs(void)
{
    screen_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.reading = &g_reading;
    in.now_ms = g_clock.now_ms;
    in.zone = &g_zone;
    in.sensors = &g_mgr;
    return in;
}

static void setup(void)
{
    fake_clock_init(&g_clock);
    fake_clock_advance(&g_clock, 10000u);
    const update_policy_config_t cfg = update_policy_config_default();  // one redraw per 5 s
    const update_policy_limits_t limits = {.min_full_refresh_interval_ms = 60000u,
                                           .max_partial_refreshes_before_full = 20u};
    update_policy_init(&g_policy, fake_clock_port(&g_clock), &cfg, &limits);
    const screen_controller_config_t sc_cfg = screen_controller_config_default();
    screen_controller_init(&g_sc, &sc_cfg);
    air_reading_init(&g_reading);
    g_reading.co2_ppm = measurement_ok(700.0f, g_clock.now_ms, 1u);
    memset(&g_mgr, 0, sizeof(g_mgr));
    g_mgr.count = 1u;
    g_mgr.slots[0].source = &g_src;
    g_mgr.slots[0].state = SENSOR_SLOT_IDLE;
    g_mgr.slots[0].ever_ready = true;
    g_mgr.slots[0].inits = 1u;
    g_zone = time_zone_eu(120);
    framebuffer_init(&g_fb, g_pixels, sizeof(g_pixels), PANEL_W, PANEL_H, PIXFMT_GRAY4);
}

// One pass of a loop: evaluate, and if due, render, "flush" (taking
// `flush_ms` on the clock) and commit. Returns the decision.
static update_decision_t pass(uint64_t flush_ms, screen_redraw_note_t *note)
{
    const screen_inputs_t in = inputs();
    update_decision_t d;
    screen_controller_evaluate(&g_sc, &g_policy, &in, &d);
    memset(note, 0, sizeof(*note));
    if (d.action != UPDATE_NONE) {
        screen_controller_render(&g_sc, &g_fb, &in);
        fake_clock_advance(&g_clock, flush_ms);
        update_policy_commit(&g_policy, &g_reading, &d);
        screen_controller_committed(&g_sc, g_clock.now_ms, note);
    }
    return d;
}

static void tap_at(uint16_t x, uint16_t y)
{
    const input_event_t down = {.kind = INPUT_TOUCH_DOWN, .x = x, .y = y, .ts_ms = g_clock.now_ms};
    (void)screen_controller_input(&g_sc, &g_policy, &down);
    fake_clock_advance(&g_clock, 100u);
    const input_event_t up = {.kind = INPUT_TOUCH_UP, .x = x, .y = y, .ts_ms = g_clock.now_ms};
    (void)screen_controller_input(&g_sc, &g_policy, &up);
}

static void a_tap_switches_the_screen_at_once_as_a_full_refresh(void)
{
    setup();
    screen_redraw_note_t note;
    update_decision_t d = pass(3300u, &note);
    HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);
    HAC_CHECK(!note.screen_changed);
    HAC_CHECK_EQ_INT(screen_controller_screen(&g_sc), SCREEN_HOME);
    uint8_t *home = malloc(sizeof(g_pixels));
    HAC_CHECK(home != NULL);
    memcpy(home, g_pixels, sizeof(g_pixels));

    // A second after the first frame -- inside the 5 s cycle a reading would
    // wait out -- a tap.
    fake_clock_advance(&g_clock, 1000u);
    const uint64_t lifted = g_clock.now_ms + 100u;
    tap_at(480u, 270u);
    HAC_CHECK_EQ_INT(screen_controller_screen(&g_sc), SCREEN_DIAGNOSTICS);
    d = pass(3300u, &note);
    HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);  // a second after the first frame's
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_REQUEST);
    HAC_CHECK_EQ_INT((int)update_policy_partials_since_full(&g_policy), 0);
    HAC_CHECK(note.screen_changed);
    HAC_CHECK(note.by_tap);
    HAC_CHECK_EQ_INT(note.screen, SCREEN_DIAGNOSTICS);
    HAC_CHECK_EQ_U64(note.latency_ms, 3300u);  // the lift to the end of the flush
    HAC_CHECK_EQ_U64(g_clock.now_ms - lifted, 3300u);
    HAC_CHECK(memcmp(home, g_pixels, sizeof(g_pixels)) != 0);  // another picture

    // Nothing it shows has changed: no redraw.
    fake_clock_advance(&g_clock, 6000u);
    g_reading.co2_ppm = measurement_ok(950.0f, g_clock.now_ms, 1u);  // the home screen's business
    d = pass(0u, &note);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);

    // A tap goes home, the home screen drawn afresh -- fully, the budget
    // started over.
    tap_at(10u, 10u);
    d = pass(3300u, &note);
    HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_REQUEST);
    HAC_CHECK_EQ_INT((int)update_policy_partials_since_full(&g_policy), 0);
    HAC_CHECK_EQ_INT(note.screen, SCREEN_HOME);
    HAC_CHECK(note.by_tap);
    free(home);
}

static void the_diagnostics_screen_redraws_when_what_it_says_changes(void)
{
    setup();
    screen_redraw_note_t note;
    (void)pass(3300u, &note);
    fake_clock_advance(&g_clock, 6000u);
    tap_at(480u, 270u);
    (void)pass(1200u, &note);

    // A source fails: a status redraw, held to the cycle like any other.
    g_mgr.slots[0].failing = true;
    g_mgr.slots[0].failures = 1u;
    fake_clock_advance(&g_clock, 1000u);
    update_decision_t d = pass(500u, &note);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
    HAC_CHECK(d.held_for_redraw_interval);
    fake_clock_advance(&g_clock, 5000u);
    d = pass(500u, &note);
    HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_STATUS);
    HAC_CHECK(!note.screen_changed);
    d = pass(500u, &note);
    HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
}

static void the_diagnostics_screen_goes_home_untouched_and_touches_keep_it(void)
{
    setup();
    screen_redraw_note_t note;
    (void)pass(3300u, &note);
    HAC_CHECK_EQ_U64(screen_controller_tick(&g_sc, &g_policy, g_clock.now_ms),
                     SCREEN_CONTROLLER_NO_DEADLINE);
    tap_at(480u, 270u);
    const uint64_t lifted = g_clock.now_ms;
    (void)pass(1200u, &note);

    uint64_t due = screen_controller_tick(&g_sc, &g_policy, g_clock.now_ms);
    HAC_CHECK_EQ_U64(due, lifted + SCREEN_CONTROLLER_DEFAULT_DIAGNOSTICS_TIMEOUT_MS);

    // A drag across the glass half way: not a tap, but a touch -- the
    // timeout starts again from it.
    fake_clock_advance(&g_clock, 30000u);
    input_event_t ev = {.kind = INPUT_TOUCH_DOWN, .x = 100u, .y = 100u, .ts_ms = g_clock.now_ms};
    (void)screen_controller_input(&g_sc, &g_policy, &ev);
    ev.kind = INPUT_TOUCH_MOVE;
    ev.x = 400u;
    (void)screen_controller_input(&g_sc, &g_policy, &ev);
    ev.kind = INPUT_TOUCH_UP;
    HAC_CHECK(!screen_controller_input(&g_sc, &g_policy, &ev));
    HAC_CHECK_EQ_INT(screen_controller_screen(&g_sc), SCREEN_DIAGNOSTICS);
    const uint64_t touched = g_clock.now_ms;
    due = screen_controller_tick(&g_sc, &g_policy, g_clock.now_ms);
    HAC_CHECK_EQ_U64(due, touched + SCREEN_CONTROLLER_DEFAULT_DIAGNOSTICS_TIMEOUT_MS);

    fake_clock_advance(&g_clock, due - g_clock.now_ms - 1u);
    HAC_CHECK_EQ_U64(screen_controller_tick(&g_sc, &g_policy, g_clock.now_ms), due);
    fake_clock_advance(&g_clock, 1u);
    HAC_CHECK_EQ_U64(screen_controller_tick(&g_sc, &g_policy, g_clock.now_ms),
                     SCREEN_CONTROLLER_NO_DEADLINE);
    HAC_CHECK_EQ_INT(screen_controller_screen(&g_sc), SCREEN_HOME);
    const update_decision_t d = pass(3300u, &note);
    HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);  // the timeout's change is full too
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_REQUEST);
    HAC_CHECK(note.screen_changed);
    HAC_CHECK(!note.by_tap);  // the timeout's, not a person's
    HAC_CHECK_EQ_INT((int)g_sc.timeouts, 1);
}

static void taps_before_the_first_frame_are_part_of_it(void)
{
    setup();
    tap_at(480u, 270u);
    screen_redraw_note_t note;
    const update_decision_t d = pass(3300u, &note);
    HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);
    HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_FIRST_FRAME);
    HAC_CHECK(note.screen_changed);
    HAC_CHECK_EQ_INT(note.screen, SCREEN_DIAGNOSTICS);
    HAC_CHECK(strcmp(screen_controller_screen_name(SCREEN_DIAGNOSTICS), "diagnostics") == 0);
}

static void firmware_update_locks_touch_and_shows_each_stage_without_waiting_for_a_cycle(void)
{
    setup();
    screen_redraw_note_t note;
    (void)pass(3300u, &note);
    tap_at(480u, 270u);
    (void)pass(3300u, &note);
    HAC_CHECK_EQ_INT(screen_controller_screen(&g_sc), SCREEN_DIAGNOSTICS);
    const uint32_t taps_before = g_sc.taps;

    // A gesture began on diagnostics before the update took over.
    const input_event_t down = {
        .kind = INPUT_TOUCH_DOWN, .x = 10u, .y = 10u, .ts_ms = g_clock.now_ms};
    (void)screen_controller_input(&g_sc, &g_policy, &down);
    firmware_update_progress_t progress = {.stage = FIRMWARE_UPDATE_CHECKING,
                                           .total_bytes = 1000000u};
    screen_controller_firmware_update(&g_sc, &g_policy, &progress, g_clock.now_ms);
    update_decision_t d = pass(0u, &note);
    HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);
    HAC_CHECK_EQ_INT(screen_controller_screen(&g_sc), SCREEN_FIRMWARE_UPDATE);
    HAC_CHECK(note.screen_changed);

    // Both a queued lift and fresh complete taps are ignored. The ordinary
    // diagnostics timeout cannot replace the update, however long it takes.
    input_event_t up = {.kind = INPUT_TOUCH_UP, .x = 10u, .y = 10u, .ts_ms = g_clock.now_ms + 1u};
    HAC_CHECK(!screen_controller_input(&g_sc, &g_policy, &up));
    fake_clock_advance(&g_clock, SCREEN_CONTROLLER_DEFAULT_DIAGNOSTICS_TIMEOUT_MS + 1u);
    HAC_CHECK_EQ_U64(screen_controller_tick(&g_sc, &g_policy, g_clock.now_ms),
                     SCREEN_CONTROLLER_NO_DEADLINE);

    const firmware_update_stage_t stages[] = {FIRMWARE_UPDATE_WRITING, FIRMWARE_UPDATE_VERIFYING,
                                              FIRMWARE_UPDATE_FINALIZING,
                                              FIRMWARE_UPDATE_RESTARTING};
    for (size_t i = 0u; i < sizeof(stages) / sizeof(stages[0]); ++i) {
        progress.stage = stages[i];
        progress.written_bytes = i == 0u ? 500000u : progress.total_bytes;
        screen_controller_firmware_update(&g_sc, &g_policy, &progress, g_clock.now_ms);
        d = pass(0u, &note);
        HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);
        HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_STATUS);
        tap_at(10u, 10u);
        HAC_CHECK_EQ_INT(screen_controller_screen(&g_sc), SCREEN_FIRMWARE_UPDATE);
        HAC_CHECK_EQ_INT((int)g_sc.taps, (int)taps_before);
        d = pass(0u, &note);
        HAC_CHECK_EQ_INT(d.action, UPDATE_NONE);
    }
}

static void firmware_update_errors_unlock_touch_without_replaying_old_gestures(void)
{
    const firmware_update_stage_t errors[] = {FIRMWARE_UPDATE_FAILED, FIRMWARE_UPDATE_REFUSED};
    for (size_t i = 0u; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        setup();
        screen_redraw_note_t note;
        firmware_update_progress_t progress = {.stage = FIRMWARE_UPDATE_CHECKING};
        screen_controller_firmware_update(&g_sc, &g_policy, &progress, g_clock.now_ms);
        // An update screen can itself be the boot's very first full frame.
        update_decision_t d = pass(0u, &note);
        HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);
        HAC_CHECK_EQ_INT(d.reason, UPDATE_REASON_FIRST_FRAME);
        tap_at(10u, 10u);
        const uint64_t old_touch_ms = g_clock.now_ms;
        fake_clock_advance(&g_clock, 1u);
        progress.stage = errors[i];
        screen_controller_firmware_update(&g_sc, &g_policy, &progress, g_clock.now_ms);
        d = pass(0u, &note);
        HAC_CHECK_EQ_INT(d.action, UPDATE_PARTIAL);

        // A whole queued tap from while the update was active, and an orphan
        // lift after it, must not dismiss the failure or switch to diagnostics.
        input_event_t ev = {.kind = INPUT_TOUCH_DOWN, .ts_ms = old_touch_ms - 50u};
        HAC_CHECK(!screen_controller_input(&g_sc, &g_policy, &ev));
        ev.kind = INPUT_TOUCH_UP;
        ev.ts_ms = old_touch_ms;
        HAC_CHECK(!screen_controller_input(&g_sc, &g_policy, &ev));
        ev.ts_ms = g_clock.now_ms + 1u;
        HAC_CHECK(!screen_controller_input(&g_sc, &g_policy, &ev));
        HAC_CHECK_EQ_INT(screen_controller_screen(&g_sc), SCREEN_FIRMWARE_UPDATE);
        fake_clock_advance(&g_clock, 2u);
        tap_at(10u, 10u);
        d = pass(0u, &note);
        HAC_CHECK_EQ_INT(d.action, UPDATE_FULL);
        HAC_CHECK_EQ_INT(screen_controller_screen(&g_sc), SCREEN_HOME);
    }
}

static void firmware_update_failure_times_out_and_full_only_panels_show_progress(void)
{
    setup();
    g_policy.limits.max_partial_refreshes_before_full = 0u;
    screen_redraw_note_t note;
    firmware_update_progress_t progress = {.stage = FIRMWARE_UPDATE_CHECKING, .total_bytes = 100u};
    screen_controller_firmware_update(&g_sc, &g_policy, &progress, g_clock.now_ms);
    HAC_CHECK_EQ_INT(pass(0u, &note).action, UPDATE_FULL);
    progress.stage = FIRMWARE_UPDATE_WRITING;
    progress.written_bytes = 50u;
    screen_controller_firmware_update(&g_sc, &g_policy, &progress, g_clock.now_ms);
    HAC_CHECK_EQ_INT(pass(0u, &note).action, UPDATE_FULL);
    progress.stage = FIRMWARE_UPDATE_FAILED;
    screen_controller_firmware_update(&g_sc, &g_policy, &progress, g_clock.now_ms);
    HAC_CHECK_EQ_INT(pass(0u, &note).action, UPDATE_FULL);
    const uint64_t due = g_clock.now_ms + SCREEN_CONTROLLER_UPDATE_ERROR_TIMEOUT_MS;
    fake_clock_advance(&g_clock, SCREEN_CONTROLLER_UPDATE_ERROR_TIMEOUT_MS - 1u);
    HAC_CHECK_EQ_U64(screen_controller_tick(&g_sc, &g_policy, g_clock.now_ms), due);
    fake_clock_advance(&g_clock, 1u);
    HAC_CHECK_EQ_U64(screen_controller_tick(&g_sc, &g_policy, g_clock.now_ms),
                     SCREEN_CONTROLLER_NO_DEADLINE);
    HAC_CHECK_EQ_INT(screen_controller_screen(&g_sc), SCREEN_HOME);
    HAC_CHECK_EQ_INT(pass(0u, &note).action, UPDATE_FULL);
}

int main(void)
{
    HAC_RUN(firmware_update_locks_touch_and_shows_each_stage_without_waiting_for_a_cycle);
    HAC_RUN(firmware_update_errors_unlock_touch_without_replaying_old_gestures);
    HAC_RUN(firmware_update_failure_times_out_and_full_only_panels_show_progress);
    HAC_RUN(a_tap_switches_the_screen_at_once_as_a_full_refresh);
    HAC_RUN(the_diagnostics_screen_redraws_when_what_it_says_changes);
    HAC_RUN(the_diagnostics_screen_goes_home_untouched_and_touches_keep_it);
    HAC_RUN(taps_before_the_first_frame_are_part_of_it);
    return hac_test_summary();
}
