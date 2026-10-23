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
//
// Since phase 4b (docs/adr/0030-telemetry-ports-and-the-reporter.md) it runs
// core/reporting/reporter.c after each poll, as the recorder's watch, and
// wakes at the earliest of three deadlines: the sensor manager's,
// update_policy's and the reporter's. The network and MQTT adapters wake it
// through event_loop_post_network() when something changed. The board
// composes a reporter since phase 4g (docs/adr/0033-mqtt-on-the-board.md),
// in an image with a broker; once an hour this logs what it published.
//
// Since phase 4d (docs/adr/0031-the-network-on-the-screen.md) it holds the
// link the reporter reports (core/app/link_hold.h) and puts the held state
// into the screen's reading, so the status bar says "online", "no broker" or
// "no Wi-Fi" once the link has held it for a minute -- and nothing without a
// reporter; the hold's end is a fourth deadline.
//
// Since phase 4e (docs/adr/0032-wifi-on-the-board.md) it calls the
// composition once the first redraw is over -- on the glass or not -- which
// is when the board starts Wi-Fi: the radio's calibration current is to come
// neither in the switched rail's inrush nor during a panel drive, and the
// first frame, a full refresh, follows the rail by a fraction of a second.
//
// Since phase 4f (docs/adr/0023's amendment, "Time from the network") it
// takes the composition's step after the reporter's: work done here in short
// steps, with a deadline of its own -- the RTC measured and written after
// each SNTP answer (net_time.c). When ADR 0022's display task takes the panel
// off this loop, the step goes with the sensors, which keep the bus.

#ifndef HAC_APPS_FIRMWARE_ESP32_EVENT_LOOP_H
#define HAC_APPS_FIRMWARE_ESP32_EVENT_LOOP_H

#include <stdint.h>

#include "app/link_hold.h"
#include "app/screen_controller.h"
#include "app/sensor_manager.h"
#include "app/update_policy.h"
#include "domain/civil_time.h"
#include "domain/measurement.h"
#include "port_clock.h"
#include "port_display.h"
#include "port_input.h"
#include "port_log.h"
#include "reporting/reporter.h"
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

// Called once, on the pump's task, when the first redraw is over and the
// panel is no longer being driven. Must not wait.
typedef void (*event_loop_first_frame_fn)(void *ctx);

// Called after every poll, the reporter's step and the link's hold: the
// composition's work that runs on this task in short steps and wants waking
// for -- phase 4f's time from the network. Returns its next deadline on
// port_clock's now_ms(), UINT64_MAX for none. May use the bus; must not
// wait longer than a few tens of milliseconds.
typedef uint64_t (*event_loop_step_fn)(void *ctx, uint64_t now_ms);

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

    reporter_t *reporter;  // may be NULL: no telemetry; already reporter_init()-ed
    link_hold_t *link;     // may be NULL: no link on the screen; already link_hold_init()-ed

    event_loop_first_frame_fn after_first_frame;  // may be NULL
    void *after_first_frame_ctx;

    event_loop_step_fn step;  // may be NULL
    void *step_ctx;
} event_loop_deps_t;

// Starts the FreeRTOS task that runs the pump: forever, it polls the sensor
// manager, steps the reporter, evaluates the policy, renders and flushes when
// a redraw is due, and sleeps until the earliest of their deadlines (clamped
// to EVENT_LOOP_MAX_WAIT_MS, so a quiet period still shows in the log).
//
// `deps` is copied into the task's own storage; the pointers it carries must
// outlive the task, which never exits. The sensors are only ever touched from
// that task. Returns false if the queues, framebuffer snapshot or task
// cannot be created; the caller must not start the card task in that case.
bool event_loop_start(const event_loop_deps_t *deps, uint32_t stack_bytes, int priority,
                      int core_id);

// The card's task reports an update milestone and waits for one display
// attempt. The pump remains the only owner of the framebuffer and panel.
// True only after the new frame reached the glass (or was already shown).
// Never call from the pump itself. Start the card only after event_loop_start.
bool event_loop_firmware_update(void *ctx, const firmware_update_progress_t *progress);

// From another task: input is waiting -- the pump looks now rather than at
// its next deadline. Never waits; a wake that does not fit the queue is
// dropped, since one already queued does the same. Safe before the pump
// runs.
void event_loop_post_input(void);

// From another task: the network or the MQTT session changed, or a message
// arrived -- the reporter looks now. As event_loop_post_input().
void event_loop_post_network(void);

// From another task: the network's time arrived, or the RTC was written by
// hand -- the step looks now. As event_loop_post_input().
void event_loop_post_time(void);

#ifdef __cplusplus
}
#endif

#endif  // HAC_APPS_FIRMWARE_ESP32_EVENT_LOOP_H
