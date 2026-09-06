// SPDX-License-Identifier: Apache-2.0
//
// The FreeRTOS pump of docs/architecture.md 6.1. FreeRTOS is named in this
// file and nowhere else under apps/firmware_esp32/main/: bringup_reading.c
// and main.c know only port_clock, port_display, port_log and
// update_policy.
//
// docs/architecture.md 6.1 designs a single-threaded core state machine
// (app_core_handle()) that the platform's event loop drives. That core does
// not exist yet (core/app/app_core.c is unwritten -- writing it here would
// be a core change ahead of the phase order). What does exist, and already
// has exactly the shape app_core_handle() would need -- "evaluate against
// now, act, report the next deadline" -- is update_policy_evaluate(). This
// pump drives that directly, the same way apps/simulator/main.c's wait loop
// already does (main.c:390-424): the seam this file sits on is unchanged
// when app_core lands and starts sitting on it instead.

#ifndef HAC_APPS_FIRMWARE_ESP32_EVENT_LOOP_H
#define HAC_APPS_FIRMWARE_ESP32_EVENT_LOOP_H

#include <stdint.h>

#include "app/update_policy.h"
#include "domain/measurement.h"
#include "port_clock.h"
#include "port_display.h"
#include "port_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fills `reading` for this iteration; `now_ms` is handed in so the source
// can derive time-varying values without owning a clock of its own. Phase
// 2a's only implementation is bringup_reading.c's synthetic sawtooth -- a
// real sensor_manager takes this seat in phase 3.
typedef void (*event_loop_reading_fn)(void *ctx, air_reading_t *reading, uint64_t now_ms);

typedef struct {
    const port_clock_t *clock;
    port_display_t *display;
    const port_log_t *log;
    update_policy_t *policy;  // already update_policy_init()-ed by the caller

    event_loop_reading_fn read_reading;
    void *read_reading_ctx;

    // Passed to air_reading_apply_age() each iteration, exactly as
    // apps/simulator/main.c ages SIM_MAX_READING_AGE_MS.
    uint32_t max_reading_age_ms;
} event_loop_deps_t;

// Starts the FreeRTOS task that runs the pump: forever, it reads this
// iteration's data, evaluates the policy, renders and flushes when a redraw
// is due, and sleeps until the policy's next deadline (clamped to
// EVENT_LOOP_MAX_WAIT_MS, so a stack-high-water-mark line still appears
// during a quiet period instead of only after minutes of silence).
//
// `deps` is copied into the task's own storage; the pointers it carries
// (clock, display, log, policy, read_reading_ctx) must outlive the task,
// which never exits.
void event_loop_start(const event_loop_deps_t *deps, uint32_t stack_bytes, int priority,
                      int core_id);

#ifdef __cplusplus
}
#endif

#endif  // HAC_APPS_FIRMWARE_ESP32_EVENT_LOOP_H
