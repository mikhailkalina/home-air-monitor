// SPDX-License-Identifier: Apache-2.0

#include "event_loop.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"

#include "ui/screen_home.h"
#include "ui/view_model.h"

#define EVENT_LOOP_TAG "event_loop"

// A due redraw's deadline is never waited on for longer than this, even when
// the policy reports UPDATE_POLICY_NO_DEADLINE, so a heartbeat and a
// stack-high-water-mark line still appear during a quiet period instead of
// only after minutes of silence.
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
// drowning the log and the CPU.
#define EVENT_LOOP_BACKOFF_FIRST_MS 1000u
#define EVENT_LOOP_BACKOFF_MAX_SHIFT 6u

#define EVENT_LOOP_QUEUE_DEPTH 8u

// What docs/architecture.md 6.1's app_event_t will eventually be. Nothing
// produces one yet in phase 2a -- there is no touch IRQ, no sensor-ready
// signal -- so the queue exists for its shape (xQueueReceive with a timeout
// computed from the policy's next deadline, exactly as the architecture
// describes) rather than for traffic; a timeout and a real event are handled
// identically below, because either one means "re-evaluate now".
typedef struct {
    uint8_t kind;
    uint64_t ts_ms;
} pump_event_t;

typedef struct {
    event_loop_deps_t deps;
    QueueHandle_t queue;
    air_reading_t reading;
    bool deferral_logged;

    // Consecutive iterations that wanted a redraw and did not get one onto the
    // glass -- a failed flush, or a diff that turned out empty. Drives the
    // backoff in wait_ms_for_deadline(); reset by any successful flush.
    uint32_t consecutive_no_progress;

    // A snapshot of what the panel was last successfully flushed with, so a
    // partial redraw can be shrunk to what actually changed instead of the
    // whole panel screen_home_render() unconditionally repaints every time.
    // Lives in PSRAM for the same reason the display's own framebuffer does
    // (docs/hardware/board_notes.md's memory budget): DIRAM has nowhere near
    // enough headroom for a second 253 KB buffer.
    framebuffer_t previous_fb;
} event_loop_state_t;

// update_decision_t::next_deadline_ms is a point on the core's own clock
// (the same epoch port_clock_t::now_ms() reads), not a duration -- see
// core/app/update_policy.h's field comment. "next_deadline_ms=49" alone reads
// as "wake up in 49 ms" to anyone who has not read that comment, when what it
// actually means is "the clock read 49 ms at the moment this fired". The
// "_abs_ms" suffix and the adjacent now_ms field name that fact instead of
// relying on the reader to already know it. The two will always be equal
// here: log_decision() only runs when update_policy_evaluate() decided a
// redraw is due *now* (update_policy.c sets next_deadline_ms = now_ms in
// that branch), so an interval field would always read 0 -- not logged for
// that reason. The deferred-redraw log below, where the deadline is a real
// future point, is where the interval is worth logging.
static void log_decision(const event_loop_state_t *s, const update_decision_t *d, uint64_t now_ms)
{
    const log_field_t fields[] = {
        log_str("action", update_policy_action_name(d->action)),
        log_str("reason", update_policy_reason_name(d->reason)),
        log_u64("partials", update_policy_partials_since_full(s->deps.policy)),
        log_bool("forced_full", d->full_forced_by_budget),
        log_u64("now_ms", now_ms),
        log_u64("next_deadline_abs_ms", d->next_deadline_ms),
    };
    port_log_write(s->deps.log, LOG_LEVEL_INFO, EVENT_LOOP_TAG, "decision", fields,
                   sizeof(fields) / sizeof(fields[0]));
}

// The evaluate -> render -> flush -> commit sequence of
// apps/simulator/main.c:253-268 (redraw()), unchanged in shape: the policy is
// only credited with the redraw once the flush actually reaches the display.
// Returns true only when the flush actually reached the panel, which is what
// the caller's backoff is keyed on.
static bool redraw(event_loop_state_t *s, const update_decision_t *d)
{
    framebuffer_t *fb = s->deps.display->get_framebuffer(s->deps.display);
    if (fb == NULL) {
        port_log_write(s->deps.log, LOG_LEVEL_ERROR, EVENT_LOOP_TAG,
                       "get_framebuffer returned NULL; skipping redraw", NULL, 0u);
        return false;
    }

    view_model_t vm;
    view_model_build(&vm, &s->reading, s->deps.clock->now_ms(s->deps.clock));
    screen_home_render(fb, &vm);

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
            // bug: statuses_differ() also fires on `charging` and
            // `telemetry_connected`, neither of which screen_home draws yet
            // (docs/hardware/board_notes.md open items), so this path can be
            // reached every tick, harmless but wasteful, until `reading`
            // changes again. Flushing anyway would spend a partial-refresh
            // budget slot for zero visible change, which is exactly what
            // that budget exists to prevent, so update_policy_commit() is
            // skipped along with the flush.
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
    return true;
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

static uint32_t wait_ms_for_deadline(uint64_t next_deadline_ms, uint64_t now_ms,
                                     uint32_t no_progress)
{
    uint32_t wait_ms;

    if (next_deadline_ms == UPDATE_POLICY_NO_DEADLINE) {
        wait_ms = EVENT_LOOP_MAX_WAIT_MS;
    } else if (next_deadline_ms <= now_ms) {
        wait_ms = 0u;
    } else {
        const uint64_t wait = next_deadline_ms - now_ms;
        wait_ms = (wait < EVENT_LOOP_MAX_WAIT_MS) ? (uint32_t)wait : EVENT_LOOP_MAX_WAIT_MS;
    }

    // The floor only ever delays a retry that just failed; it never defers a
    // deadline that is genuinely in the future further than it already is.
    const uint32_t floor_ms = backoff_floor_ms(no_progress);
    return (wait_ms > floor_ms) ? wait_ms : floor_ms;
}

static void pump_task(void *arg)
{
    event_loop_state_t *s = (event_loop_state_t *)arg;

    air_reading_init(&s->reading);

    for (;;) {
        const uint64_t now_ms = s->deps.clock->now_ms(s->deps.clock);

        s->deps.read_reading(s->deps.read_reading_ctx, &s->reading, now_ms);
        s->reading.uptime_s = (uint32_t)(now_ms / 1000u);
        air_reading_apply_age(&s->reading, now_ms, s->deps.max_reading_age_ms);

        update_decision_t decision;
        update_policy_evaluate(s->deps.policy, &s->reading, &decision);

        if (decision.action != UPDATE_NONE) {
            log_decision(s, &decision, now_ms);
            if (redraw(s, &decision)) {
                s->consecutive_no_progress = 0u;
            } else if (s->consecutive_no_progress < UINT32_MAX) {
                s->consecutive_no_progress++;
            }
        } else if (decision.deferred_by_min_interval && !s->deferral_logged) {
            // Said once per episode, like the simulator's equivalent line
            // (apps/simulator/main.c:416-423): repeating it every tick would
            // bury everything else in the log.
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

        const uint32_t wait_ms =
            wait_ms_for_deadline(decision.next_deadline_ms, now_ms, s->consecutive_no_progress);

        // uxTaskGetStackHighWaterMark() returns words remaining, not bytes,
        // on every ESP-IDF FreeRTOS port; logged every iteration so the
        // 6144-byte stack budget can be revisited from evidence rather than
        // kept out of superstition.
        const UBaseType_t high_water_words = uxTaskGetStackHighWaterMark(NULL);
        const log_field_t tick_fields[] = {
            log_u64("stack_high_water_words", high_water_words),
            log_u64("wait_ms", wait_ms),
            log_u64("no_progress", s->consecutive_no_progress),
        };
        port_log_write(s->deps.log, LOG_LEVEL_DEBUG, EVENT_LOOP_TAG, "tick", tick_fields,
                       sizeof(tick_fields) / sizeof(tick_fields[0]));

        // A timeout and a genuine queued event both mean the same thing here
        // ("re-evaluate now"), so the received event, if any, is discarded.
        // At least one tick is always waited: a policy that reports "due
        // now" every single iteration would otherwise spin this task at
        // 100% CPU and starve the idle task, tripping the watchdog instead
        // of reaching the log line that would explain why.
        const TickType_t raw_ticks = pdMS_TO_TICKS(wait_ms);
        const TickType_t wait_ticks = (raw_ticks > 0u) ? raw_ticks : (TickType_t)1u;

        pump_event_t ev;
        (void)xQueueReceive(s->queue, &ev, wait_ticks);
    }
}

void event_loop_start(const event_loop_deps_t *deps, uint32_t stack_bytes, int priority,
                      int core_id)
{
    // Outlives pump_task, which never returns; a static rather than a heap
    // allocation, matching the "no core/ allocation" style even though this
    // file is firmware composition, not core/.
    static event_loop_state_t s_state;

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
