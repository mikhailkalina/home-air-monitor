// SPDX-License-Identifier: Apache-2.0
//
// The FreeRTOS pump of docs/architecture.md 6.1. FreeRTOS is named in this
// file and nowhere else under apps/firmware_esp32/main/: main.c knows only
// the ports, the sensor manager and update_policy.
//
// docs/architecture.md 6.1 designs a single-threaded core state machine
// (app_core_handle()) that the platform's event loop drives. That core does
// not exist yet (core/app/app_core.c is unwritten -- writing it here would
// be a core change ahead of the phase order). What does exist, and already
// has exactly the shape app_core_handle() would need -- "act on what is due
// now, report the next deadline" -- is the pair sensor_manager_poll() and
// update_policy_evaluate(). This pump drives both, the same way
// apps/simulator/main.c's loop does, and sleeps until the earlier of their
// deadlines: the seam this file sits on is unchanged when app_core lands and
// starts sitting on it instead.
//
// Since phase 3g (docs/adr/0025-touch-input-gt911.md) it also drains
// port_input into core/app/screen_controller.c, which says which screen the
// redraw draws and when the diagnostics screen times out; the touch adapter
// wakes the pump through event_loop_post_input() when it has queued events,
// and a redraw that carried a tap is logged with the time from the finger's
// lift to the end of the flush.

#ifndef HAC_APPS_FIRMWARE_ESP32_EVENT_LOOP_H
#define HAC_APPS_FIRMWARE_ESP32_EVENT_LOOP_H

#include <stdint.h>

#include "app/screen_controller.h"
#include "app/sensor_manager.h"
#include "app/update_policy.h"
#include "domain/civil_time.h"
#include "domain/measurement.h"
#include "port_clock.h"
#include "port_display.h"
#include "port_input.h"
#include "port_log.h"
#include "ui/diagnostics_view.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called after every poll with the merged reading: the composition's hook for
// what belongs to neither the core nor this pump -- handing the measured
// temperature to the display adapter, logging the sensors' samples.
typedef void (*event_loop_reading_fn)(void *ctx, const sensor_manager_t *sensors,
                                      const air_reading_t *reading, uint64_t now_ms);

// What the diagnostics screen shows that only the composition knows: the
// recorder's state, and where the wall clock came from (a word, or NULL).
typedef void (*event_loop_facts_fn)(void *ctx, diag_recorder_t *recorder,
                                    const char **clock_source);

typedef struct {
    const port_clock_t *clock;
    port_display_t *display;
    const port_log_t *log;
    update_policy_t *policy;    // already update_policy_init()-ed by the caller
    sensor_manager_t *sensors;  // already sensor_manager_init()-ed, sources added
    const time_zone_t *zone;    // the local time the footer shows

    event_loop_reading_fn on_reading;  // may be NULL
    void *on_reading_ctx;

    port_input_t *input;           // may be NULL: no touch
    screen_controller_t *screens;  // already screen_controller_init()-ed
    event_loop_facts_fn facts;     // may be NULL
    void *facts_ctx;
} event_loop_deps_t;

// Starts the FreeRTOS task that runs the pump: forever, it polls the sensor
// manager, evaluates the policy, renders and flushes when a redraw is due,
// and sleeps until the earlier of the two deadlines (clamped to
// EVENT_LOOP_MAX_WAIT_MS, so a quiet period still shows in the log).
//
// `deps` is copied into the task's own storage; the pointers it carries must
// outlive the task, which never exits. The sensors are only ever touched from
// that task.
void event_loop_start(const event_loop_deps_t *deps, uint32_t stack_bytes, int priority,
                      int core_id);

// From another task: input is waiting -- the pump looks now rather than at
// its next deadline. Never waits; a wake that does not fit the queue is
// dropped, since one already queued does the same. Safe before the pump
// runs.
void event_loop_post_input(void);

#ifdef __cplusplus
}
#endif

#endif  // HAC_APPS_FIRMWARE_ESP32_EVENT_LOOP_H
