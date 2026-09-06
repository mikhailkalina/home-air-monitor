// SPDX-License-Identifier: Apache-2.0

#include "event_loop.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ui/screen_home.h"
#include "ui/view_model.h"

#define EVENT_LOOP_TAG "event_loop"

// A due redraw's deadline is never waited on for longer than this, even when
// the policy reports UPDATE_POLICY_NO_DEADLINE, so a heartbeat and a
// stack-high-water-mark line still appear during a quiet period instead of
// only after minutes of silence.
#define EVENT_LOOP_MAX_WAIT_MS 60000u

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
} event_loop_state_t;

static void log_decision(const event_loop_state_t *s, const update_decision_t *d)
{
    const log_field_t fields[] = {
        log_str("action", update_policy_action_name(d->action)),
        log_str("reason", update_policy_reason_name(d->reason)),
        log_u64("partials", update_policy_partials_since_full(s->deps.policy)),
        log_bool("forced_full", d->full_forced_by_budget),
        log_u64("next_deadline_ms", d->next_deadline_ms),
    };
    port_log_write(s->deps.log, LOG_LEVEL_INFO, EVENT_LOOP_TAG, "decision", fields,
                   sizeof(fields) / sizeof(fields[0]));
}

// The evaluate -> render -> flush -> commit sequence of
// apps/simulator/main.c:253-268 (redraw()), unchanged in shape: the policy is
// only credited with the redraw once the flush actually reaches the display.
static void redraw(event_loop_state_t *s, const update_decision_t *d)
{
    framebuffer_t *fb = s->deps.display->get_framebuffer(s->deps.display);
    if (fb == NULL) {
        port_log_write(s->deps.log, LOG_LEVEL_ERROR, EVENT_LOOP_TAG,
                       "get_framebuffer returned NULL; skipping redraw", NULL, 0u);
        return;
    }

    view_model_t vm;
    view_model_build(&vm, &s->reading, s->deps.clock->now_ms(s->deps.clock));
    screen_home_render(fb, &vm);

    const refresh_mode_t mode = (d->action == UPDATE_FULL) ? REFRESH_FULL : REFRESH_PARTIAL;
    if (s->deps.display->flush(s->deps.display, NULL, mode) == HAL_OK) {
        update_policy_commit(s->deps.policy, &s->reading, d);
        s->deferral_logged = false;
    }
}

static uint32_t wait_ms_for_deadline(uint64_t next_deadline_ms, uint64_t now_ms)
{
    if (next_deadline_ms == UPDATE_POLICY_NO_DEADLINE) {
        return EVENT_LOOP_MAX_WAIT_MS;
    }
    if (next_deadline_ms <= now_ms) {
        return 0u;
    }
    const uint64_t wait = next_deadline_ms - now_ms;
    return (wait < EVENT_LOOP_MAX_WAIT_MS) ? (uint32_t)wait : EVENT_LOOP_MAX_WAIT_MS;
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
            log_decision(s, &decision);
            redraw(s, &decision);
        } else if (decision.deferred_by_min_interval && !s->deferral_logged) {
            // Said once per episode, like the simulator's equivalent line
            // (apps/simulator/main.c:416-423): repeating it every tick would
            // bury everything else in the log.
            const log_field_t fields[] = {
                log_str("reason", update_policy_reason_name(decision.reason)),
                log_u64("next_deadline_ms", decision.next_deadline_ms),
            };
            port_log_write(s->deps.log, LOG_LEVEL_INFO, EVENT_LOOP_TAG,
                           "deferred: panel minimum full-refresh interval not elapsed", fields,
                           sizeof(fields) / sizeof(fields[0]));
            s->deferral_logged = true;
        }

        const uint32_t wait_ms = wait_ms_for_deadline(decision.next_deadline_ms, now_ms);

        // uxTaskGetStackHighWaterMark() returns words remaining, not bytes,
        // on every ESP-IDF FreeRTOS port; logged every iteration so the
        // 6144-byte stack budget can be revisited from evidence rather than
        // kept out of superstition.
        const UBaseType_t high_water_words = uxTaskGetStackHighWaterMark(NULL);
        const log_field_t tick_fields[] = {
            log_u64("stack_high_water_words", high_water_words),
            log_u64("wait_ms", wait_ms),
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
