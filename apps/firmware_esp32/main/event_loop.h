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

#ifndef HAC_APPS_FIRMWARE_ESP32_EVENT_LOOP_H
#define HAC_APPS_FIRMWARE_ESP32_EVENT_LOOP_H

#include <stdint.h>

#include "app/sensor_manager.h"
#include "app/update_policy.h"
#include "domain/civil_time.h"
#include "domain/measurement.h"
#include "port_clock.h"
#include "port_display.h"
#include "port_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called after every poll with the merged reading: the composition's hook for
// what belongs to neither the core nor this pump -- handing the measured
// temperature to the display adapter, logging the sensors' samples.
typedef void (*event_loop_reading_fn)(void *ctx, const sensor_manager_t *sensors,
                                      const air_reading_t *reading, uint64_t now_ms);

typedef struct {
    const port_clock_t *clock;
    port_display_t *display;
    const port_log_t *log;
    update_policy_t *policy;    // already update_policy_init()-ed by the caller
    sensor_manager_t *sensors;  // already sensor_manager_init()-ed, sources added
    const time_zone_t *zone;    // the local time the footer shows

    event_loop_reading_fn on_reading;  // may be NULL
    void *on_reading_ctx;
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

#ifdef __cplusplus
}
#endif

#endif  // HAC_APPS_FIRMWARE_ESP32_EVENT_LOOP_H
