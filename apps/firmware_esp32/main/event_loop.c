// SPDX-License-Identifier: Apache-2.0

#include "event_loop.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"

#define EVENT_LOOP_TAG "event_loop"

// No deadline is waited on for longer than this, even when neither the
// policy nor the sensor manager has one, so the pump still wakes during a
// quiet period instead of only after minutes of silence.
#define EVENT_LOOP_MAX_WAIT_MS 60000u

// Backoff after an iteration that wanted a redraw and did not achieve one.
//
// Without it the pump spins at the FreeRTOS tick rate: update_policy_commit()
// is deliberately skipped when the flush fails, so the decision stays "due
// now", so the next iteration retries immediately, forever. That is not
// hypothetical -- it is what the first EPDiy flash did, several hundred
// failing full refreshes per minute, each with its own log line, for as long
// as the board was powered.
//
// Doubling from one second to the same ceiling the idle wait uses. A failure
// that clears itself still recovers within a second; one that does not stops
// drowning the log and the CPU. Since phase 3c the pump also wakes for the
// sensors every few seconds, so the backoff is a point before which no redraw
// is attempted (redraw_not_before_ms), not merely a longer sleep.
#define EVENT_LOOP_BACKOFF_FIRST_MS 1000u
#define EVENT_LOOP_BACKOFF_MAX_SHIFT 6u

#define EVENT_LOOP_QUEUE_DEPTH 8u

// What docs/architecture.md 6.1's app_event_t will eventually be. Since
// phase 3g the touch adapter posts one when it has queued input
// (event_loop_post_input()), since phase 4b the network and MQTT adapters
// when something changed (event_loop_post_network()); the sensors are still
// polled at the sensor manager's deadlines rather than signalled. A timeout and a real event are
// handled identically below, because either one means "re-evaluate now":
// the input itself waits in the adapter's queue, with its own time.
typedef struct {
    uint8_t kind;
    uint64_t ts_ms;
} pump_event_t;

#define PUMP_EVENT_INPUT 1u
#define PUMP_EVENT_NETWORK 2u

typedef struct {
    event_loop_deps_t deps;
    QueueHandle_t queue;
    air_reading_t reading;
    bool deferral_logged;

    // Consecutive iterations that wanted a redraw and did not get one onto the
    // glass -- a failed flush, or a diff that turned out empty -- and the
    // point on the clock before which no redraw is tried again. Reset by any
    // successful flush.
    uint32_t consecutive_no_progress;
    uint64_t redraw_not_before_ms;

    // The least free stack this task has had, in bytes, as last logged.
    uint32_t stack_free_min_bytes;

    // The first redraw is over, and the composition has been told.
    bool first_frame_done;

    // What the reporter and its network said at the last step: the
    // diagnostics screen's, and the link the hold is fed.
    reporter_status_t network;

    // A snapshot of what the panel was last successfully flushed with, so a
    // partial redraw can be shrunk to what actually changed instead of the
    // whole panel a screen's render unconditionally repaints every time.
    // Lives in PSRAM for the same reason the display's own framebuffer does
    // (docs/hardware/board_notes.md's memory budget): DIRAM has nowhere near
    // enough headroom for a second 253 KB buffer.
    framebuffer_t previous_fb;
} event_loop_state_t;

// Outlives pump_task, which never returns; a static rather than a heap
// allocation, matching the "no core/ allocation" style even though this file
// is firmware composition, not core/. At file scope for
// event_loop_post_input(), which other tasks call.
static event_loop_state_t s_state;

// What the screens draw from, at `now_ms` -- the time the reading's wall
// time was read at.
static screen_inputs_t screen_inputs(const event_loop_state_t *s, uint64_t now_ms)
{
    screen_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.reading = &s->reading;
    in.now_ms = now_ms;
    in.zone = s->deps.zone;
    in.sensors = s->deps.sensors;
    if (s->deps.facts != NULL) {
        s->deps.facts(s->deps.facts_ctx, &in.recorder, &in.clock_source);
    }
    in.network = (s->deps.reporter != NULL) ? &s->network : NULL;
    return in;
}

// The link as it is now, held before the screen shows it
// (core/app/link_hold.h); a change of what the bar says is logged, with what
// the link has been doing meanwhile in the reporter's own lines. Returns the
// hold's deadline.
static uint64_t hold_link(event_loop_state_t *s, uint64_t now_ms)
{
    reporter_status(s->deps.reporter, &s->network);
    if (s->deps.link == NULL) {
        s->reading.link = LINK_OFF;
        return LINK_HOLD_NO_DEADLINE;
    }
    bool changed = false;
    const uint64_t due = link_hold_step(s->deps.link, s->network.link, now_ms, &changed);
    s->reading.link = link_hold_shown(s->deps.link);
    if (changed) {
        const log_field_t fields[] = {
            log_str("shown", link_status_name(s->reading.link)),
            log_u64("changes", s->deps.link->changes),
            log_u64("now_ms", now_ms),
        };
        port_log_write(s->deps.log, LOG_LEVEL_INFO, EVENT_LOOP_TAG, "link on the screen", fields,
                       sizeof(fields) / sizeof(fields[0]));
    }
    return due;
}

// A redraw that put a new screen on the glass: which, and, for a tap, how
// long from the finger's lift to the end of the flush -- the latency a
// person sees, measured on port_clock across the whole synchronous flush.
static void log_screen_change(const event_loop_state_t *s, const screen_redraw_note_t *note,
                              const update_decision_t *d)
{
    if (!note->screen_changed) {
        return;
    }
    const log_field_t fields[] = {
        log_str("screen", screen_controller_screen_name(note->screen)),
        log_str("by", note->by_tap ? "tap" : "timeout"),
        log_u64("latency_ms", note->by_tap ? note->latency_ms : 0u),
        log_str("action", update_policy_action_name(d->action)),
        log_u64("partials", update_policy_partials_since_full(s->deps.policy)),
    };
    port_log_write(s->deps.log, LOG_LEVEL_INFO, EVENT_LOOP_TAG, "screen changed", fields,
                   sizeof(fields) / sizeof(fields[0]));
}

// Every event the touch adapter has queued, to the screen controller.
static void drain_input(event_loop_state_t *s)
{
    if (s->deps.input == NULL || s->deps.screens == NULL) {
        return;
    }
    input_event_t ev;
    while (s->deps.input->poll(s->deps.input, &ev)) {
        if (screen_controller_input(s->deps.screens, s->deps.policy, &ev)) {
            const log_field_t fields[] = {
                log_str("next",
                        screen_controller_screen_name(screen_controller_screen(s->deps.screens))),
                log_u64("x", ev.x),
                log_u64("y", ev.y),
                log_u64("up_ms", ev.ts_ms),
            };
            port_log_write(s->deps.log, LOG_LEVEL_INFO, EVENT_LOOP_TAG, "tap", fields,
                           sizeof(fields) / sizeof(fields[0]));
        }
    }
}

// One line per redraw decision. No deadline field: log_decision() runs only
// when update_policy_evaluate() decided a redraw is due *now*, so its
// next_deadline_ms always equals now_ms here. past_budget marks a partial
// refresh that goes past the panel's budget while the full one waits for its
// minimum interval (docs/adr/0019-partials-past-the-budget.md); the full
// refresh that follows says reason=panel when nothing else changed.
static void log_decision(const event_loop_state_t *s, const update_decision_t *d, uint64_t now_ms)
{
    const log_field_t fields[] = {
        log_str("action", update_policy_action_name(d->action)),
        log_str("reason", update_policy_reason_name(d->reason)),
        log_u64("partials", update_policy_partials_since_full(s->deps.policy)),
        log_bool("forced_full", d->full_forced_by_budget),
        log_bool("past_budget", d->partial_past_budget),
        log_u64("now_ms", now_ms),
    };
    port_log_write(s->deps.log, LOG_LEVEL_INFO, EVENT_LOOP_TAG, "decision", fields,
                   sizeof(fields) / sizeof(fields[0]));
}

// What a reporter step did, as the simulator prints it: a state message at
// DEBUG -- one a minute at least, which the card's log need not hold -- and
// a session's announcements at INFO. The reporter logs its sessions itself.
static void log_report(const event_loop_state_t *s, const reporter_note_t *note, uint64_t now_ms)
{
    if (note->online_published || note->discovery_published > 0u) {
        const log_field_t fields[] = {
            log_bool("online", note->online_published),
            log_u64("discovery", note->discovery_published),
            log_u64("session", note->session),
            log_u64("now_ms", now_ms),
        };
        port_log_write(s->deps.log, LOG_LEVEL_INFO, EVENT_LOOP_TAG, "published", fields,
                       sizeof(fields) / sizeof(fields[0]));
    }
    if (note->state_published) {
        const log_field_t fields[] = {
            log_str("reason", publish_policy_reason_name(note->reason)),
            log_u64("session", note->session),
            log_u64("now_ms", now_ms),
        };
        port_log_write(s->deps.log, LOG_LEVEL_DEBUG, EVENT_LOOP_TAG, "state published", fields,
                       sizeof(fields) / sizeof(fields[0]));
    }
}

// The evaluate -> render -> flush -> commit sequence of apps/simulator/main.c's
// redraw(), unchanged in shape: the policy is only credited with the redraw
// once the flush actually reaches the display.
// Returns true only when the flush actually reached the panel, which is what
// the caller's backoff is keyed on. `now_ms` is the time the reading's wall
// time was read at (view_model_build()).
static bool redraw(event_loop_state_t *s, const update_decision_t *d, uint64_t now_ms)
{
    framebuffer_t *fb = s->deps.display->get_framebuffer(s->deps.display);
    if (fb == NULL) {
        port_log_write(s->deps.log, LOG_LEVEL_ERROR, EVENT_LOOP_TAG,
                       "get_framebuffer returned NULL; skipping redraw", NULL, 0u);
        return false;
    }

    const screen_inputs_t in = screen_inputs(s, now_ms);
    screen_controller_render(s->deps.screens, fb, &in);

    const bool full = (d->action == UPDATE_FULL);
    const refresh_mode_t mode = full ? REFRESH_FULL : REFRESH_PARTIAL;

    // NULL still means "whole panel" (the port contract), and is still the
    // right thing for a full refresh: there is no previous image a partial
    // diff could even be measured against on the first frame, and a forced
    // full repaints everything regardless. Only a partial redraw benefits
    // from shrinking to what actually changed.
    rect_t dirty_rect;
    const rect_t *area = NULL;
    if (!full) {
        if (!framebuffer_diff_dirty_rect(fb, &s->previous_fb, &dirty_rect)) {
            // update_policy decided a redraw was due, but nothing that
            // actually reaches the glass moved. This is expected, not a
            // bug: statuses_differ() also fires on `charging`, which
            // screen_home does not draw yet (docs/hardware/board_notes.md
            // open items), so this path can be reached every tick, harmless
            // but wasteful, until `reading` changes again. Flushing anyway
            // would spend a partial-refresh budget slot for zero visible
            // change, which is exactly what that budget exists to prevent,
            // so update_policy_commit() is skipped along with the flush.
            port_log_write(s->deps.log, LOG_LEVEL_INFO, EVENT_LOOP_TAG,
                           "redraw skipped: partial refresh diff was empty", NULL, 0u);
            return false;
        }
        area = &dirty_rect;
    }

    const hal_status_t status = s->deps.display->flush(s->deps.display, area, mode);
    if (status != HAL_OK) {
        const log_field_t fields[] = {
            log_i64("status", (int64_t)status),
            log_u64("consecutive_failures", (uint64_t)s->consecutive_no_progress + 1u),
        };
        port_log_write(s->deps.log, LOG_LEVEL_ERROR, EVENT_LOOP_TAG, "flush failed", fields,
                       sizeof(fields) / sizeof(fields[0]));
        return false;
    }

    update_policy_commit(s->deps.policy, &s->reading, d);
    memcpy(s->previous_fb.pixels, fb->pixels, fb->pixels_size);
    s->deferral_logged = false;
    screen_redraw_note_t note;
    screen_controller_committed(s->deps.screens, s->deps.clock->now_ms(s->deps.clock), &note);
    log_screen_change(s, &note, d);
    return true;
}

// The first redraw is over -- a full refresh, unless the flush failed -- and
// the panel idle: the composition's hook, once (event_loop.h). Logged with
// the time, which the board's "radio started" line is read against.
static void after_first_frame(event_loop_state_t *s)
{
    if (s->first_frame_done) {
        return;
    }
    s->first_frame_done = true;
    if (s->deps.after_first_frame == NULL) {
        return;
    }
    const log_field_t fields[] = {log_u64("now_ms", s->deps.clock->now_ms(s->deps.clock))};
    port_log_write(s->deps.log, LOG_LEVEL_INFO, EVENT_LOOP_TAG, "first frame over", fields, 1u);
    s->deps.after_first_frame(s->deps.after_first_frame_ctx);
}

// The backoff floor after `failures` consecutive iterations that achieved no
// redraw: 0 while things are working, then 1 s doubling to
// EVENT_LOOP_MAX_WAIT_MS.
static uint32_t backoff_floor_ms(uint32_t failures)
{
    if (failures == 0u) {
        return 0u;
    }

    const uint32_t shift = ((failures - 1u) < EVENT_LOOP_BACKOFF_MAX_SHIFT)
                               ? (failures - 1u)
                               : EVENT_LOOP_BACKOFF_MAX_SHIFT;
    const uint32_t floor_ms = EVENT_LOOP_BACKOFF_FIRST_MS << shift;

    return (floor_ms < EVENT_LOOP_MAX_WAIT_MS) ? floor_ms : EVENT_LOOP_MAX_WAIT_MS;
}

static uint32_t wait_ms_until(uint64_t wake_at_ms, uint64_t now_ms)
{
    if (wake_at_ms <= now_ms) {
        return 0u;
    }
    const uint64_t wait = wake_at_ms - now_ms;  // NO_DEADLINE lands here too, and is clamped
    return (wait < EVENT_LOOP_MAX_WAIT_MS) ? (uint32_t)wait : EVENT_LOOP_MAX_WAIT_MS;
}

// The task's least free stack so far, logged at INFO each time it reaches a
// new low -- so the figure the stack size is chosen from is in the ordinary
// boot log, and a quiet run adds no lines once it has settled.
// uxTaskGetStackHighWaterMark() counts StackType_t, which ESP-IDF's Xtensa
// port defines as uint8_t (portmacro.h): bytes, not 32-bit words.
static void log_stack_low(event_loop_state_t *s)
{
    const uint32_t free_bytes = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    if (free_bytes >= s->stack_free_min_bytes) {
        return;
    }
    s->stack_free_min_bytes = free_bytes;
    const log_field_t fields[] = {log_u64("stack_free_min_bytes", free_bytes)};
    port_log_write(s->deps.log, LOG_LEVEL_INFO, EVENT_LOOP_TAG, "stack low-water mark", fields, 1u);
}

static void pump_task(void *arg)
{
    event_loop_state_t *s = (event_loop_state_t *)arg;
    const port_clock_t *clock = s->deps.clock;

    air_reading_init(&s->reading);
    s->stack_free_min_bytes = UINT32_MAX;

    for (;;) {
        // Touch first: a tap is a redraw due now, whatever else is.
        drain_input(s);

        const uint64_t sensors_due = sensor_manager_poll(s->deps.sensors, &s->reading);

        // The time after the poll, not before it: the drivers wait out their
        // sensors' command times inside it, and a reading stamped later than
        // the time it is aged against would count as stale
        // (sensor_manager_poll()).
        // The wall clock at the same point -- ESP-IDF's system time, 0 until
        // the RTC or the console has set it -- so that a value's wall time is
        // the reading's minus its age (view_model_build()).
        const uint64_t now_ms = clock->now_ms(clock);
        s->reading.wall_time_ms = clock->wall_ms(clock);
        s->reading.uptime_s = (uint32_t)(now_ms / 1000u);
        air_reading_apply_age(&s->reading, now_ms, s->deps.sensors->cfg.max_age_ms);
        if (s->deps.on_reading != NULL) {
            s->deps.on_reading(s->deps.on_reading_ctx, s->deps.sensors, &s->reading, now_ms);
        }

        // The reporter, after the poll as the recorder's watch: the reading
        // as the screen has it, which the reporter never writes to.
        uint64_t report_due = REPORTER_NO_DEADLINE;
        if (s->deps.reporter != NULL) {
            reporter_note_t note;
            report_due = reporter_step(s->deps.reporter, &s->reading, now_ms, &note);
            log_report(s, &note, now_ms);
        }
        const uint64_t link_due = hold_link(s, now_ms);

        const uint64_t screens_due =
            screen_controller_tick(s->deps.screens, s->deps.policy, now_ms);
        const screen_inputs_t in = screen_inputs(s, now_ms);

        update_decision_t decision;
        screen_controller_evaluate(s->deps.screens, s->deps.policy, &in, &decision);

        if (decision.action != UPDATE_NONE) {
            if (now_ms >= s->redraw_not_before_ms) {
                log_decision(s, &decision, now_ms);
                if (redraw(s, &decision, now_ms)) {
                    s->consecutive_no_progress = 0u;
                    s->redraw_not_before_ms = 0u;
                } else {
                    if (s->consecutive_no_progress < UINT32_MAX) {
                        s->consecutive_no_progress++;
                    }
                    s->redraw_not_before_ms =
                        clock->now_ms(clock) + backoff_floor_ms(s->consecutive_no_progress);
                }
                after_first_frame(s);
                // What is due now that the redraw is done -- or still due,
                // if it was not.
                screen_controller_evaluate(s->deps.screens, s->deps.policy, &in, &decision);
            }
        } else if (decision.deferred_by_min_interval && !s->deferral_logged) {
            // Only a panel with no partial mode gets here (ADR 0019). Said
            // once per episode, like the simulator's equivalent line:
            // repeating it every tick would bury everything else in the log.
            //
            // Unlike log_decision()'s always-now deadline, this one is a real
            // future point (the panel's min-full-refresh-interval elapsing),
            // so next_deadline_in_ms is the useful field here: it is what
            // says how long the deferral will last, without the reader doing
            // the now_ms subtraction by hand.
            const log_field_t fields[] = {
                log_str("reason", update_policy_reason_name(decision.reason)),
                log_u64("now_ms", now_ms),
                log_u64("next_deadline_abs_ms", decision.next_deadline_ms),
                log_u64("next_deadline_in_ms", decision.next_deadline_ms - now_ms),
            };
            port_log_write(s->deps.log, LOG_LEVEL_INFO, EVENT_LOOP_TAG,
                           "deferred: panel minimum full-refresh interval not elapsed", fields,
                           sizeof(fields) / sizeof(fields[0]));
            s->deferral_logged = true;
        }

        // The earliest of the deadlines; a redraw still due waits out its
        // backoff, while the sensors and the reporter keep their own pace.
        uint64_t policy_due = decision.next_deadline_ms;
        if (decision.action != UPDATE_NONE && policy_due < s->redraw_not_before_ms) {
            policy_due = s->redraw_not_before_ms;
        }
        uint64_t wake_at = (sensors_due < policy_due) ? sensors_due : policy_due;
        if (screens_due < wake_at) {
            wake_at = screens_due;
        }
        if (report_due < wake_at) {
            wake_at = report_due;
        }
        if (link_due < wake_at) {
            wake_at = link_due;
        }
        const uint32_t wait_ms = wait_ms_until(wake_at, clock->now_ms(clock));

        log_stack_low(s);
        const log_field_t tick_fields[] = {
            log_u64("wait_ms", wait_ms),
            log_u64("sensors_due_abs_ms", sensors_due),
            log_u64("no_progress", s->consecutive_no_progress),
        };
        port_log_write(s->deps.log, LOG_LEVEL_DEBUG, EVENT_LOOP_TAG, "tick", tick_fields,
                       sizeof(tick_fields) / sizeof(tick_fields[0]));

        // A timeout and a genuine queued event both mean the same thing here
        // ("re-evaluate now"), so the received event, if any, is discarded.
        // At least one tick is always waited: a deadline that is "due now"
        // every single iteration would otherwise spin this task at 100% CPU
        // and starve the idle task, tripping the watchdog instead of reaching
        // the log line that would explain why.
        const TickType_t raw_ticks = pdMS_TO_TICKS(wait_ms);
        const TickType_t wait_ticks = (raw_ticks > 0u) ? raw_ticks : (TickType_t)1u;

        pump_event_t ev;
        (void)xQueueReceive(s->queue, &ev, wait_ticks);
    }
}

void event_loop_start(const event_loop_deps_t *deps, uint32_t stack_bytes, int priority,
                      int core_id)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.deps = *deps;
    s_state.queue = xQueueCreate(EVENT_LOOP_QUEUE_DEPTH, sizeof(pump_event_t));
    if (s_state.queue == NULL) {
        // pump_task calls xQueueReceive() on this handle every iteration; a
        // NULL handle there is worse than never starting the task at all, so
        // this returns instead of creating a task doomed to fault on its
        // first pass through the loop.
        port_log_write(s_state.deps.log, LOG_LEVEL_ERROR, EVENT_LOOP_TAG, "xQueueCreate failed",
                       NULL, 0u);
        return;
    }

    const size_t previous_fb_size = framebuffer_required_size(
        deps->display->width, deps->display->height, deps->display->format);
    uint8_t *previous_fb_pixels =
        heap_caps_malloc(previous_fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (previous_fb_pixels == NULL) {
        // Without this buffer, redraw() has nothing to diff a partial
        // refresh against; failing loudly here beats starting a pump that
        // would silently skip every partial redraw from its first tick.
        port_log_write(s_state.deps.log, LOG_LEVEL_ERROR, EVENT_LOOP_TAG,
                       "heap_caps_malloc failed for the previous-frame diff buffer", NULL, 0u);
        return;
    }
    framebuffer_init(&s_state.previous_fb, previous_fb_pixels, previous_fb_size,
                     deps->display->width, deps->display->height, deps->display->format);

    // xTaskCreatePinnedToCore's stack-size parameter is in BYTES on every
    // ESP-IDF FreeRTOS port (unlike vanilla FreeRTOS, where it is in words of
    // StackType_t) -- passed through unconverted on purpose.
    const BaseType_t created =
        xTaskCreatePinnedToCore(pump_task, "hac_pump", stack_bytes, &s_state, (UBaseType_t)priority,
                                NULL, (BaseType_t)core_id);

    // A failure here (heap exhaustion sizing the stack, most plausibly) would
    // otherwise be a silent hang: app_main() returns, no pump ever runs, and
    // nothing explains why. Logged through the same port_log the pump itself
    // would have used, so it lands in the same boot log either way.
    if (created != pdPASS) {
        const log_field_t fields[] = {log_i64("result", (int64_t)created)};
        port_log_write(s_state.deps.log, LOG_LEVEL_ERROR, EVENT_LOOP_TAG,
                       "xTaskCreatePinnedToCore failed", fields, 1u);
    }
}

static void post(uint8_t kind)
{
    if (s_state.queue == NULL) {
        return;
    }
    const pump_event_t ev = {.kind = kind, .ts_ms = 0u};
    (void)xQueueSend(s_state.queue, &ev, 0);
}

void event_loop_post_input(void)
{
    post(PUMP_EVENT_INPUT);
}

void event_loop_post_network(void)
{
    post(PUMP_EVENT_NETWORK);
}
