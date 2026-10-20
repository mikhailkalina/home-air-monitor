// SPDX-License-Identifier: Apache-2.0
//
// port_input over the GT911 (docs/adr/0025-touch-input-gt911.md). It lives
// beside the driver, not in platform/, for the reason the sensor sources do
// (docs/adr/0014-sensor-sources-live-in-drivers.md): what it decides -- when
// to read, what a report means, when a finger that stopped being reported
// has lifted -- is a host test against the GT911 model, and the board runs
// the same code. What is the board's alone, the interrupt and the task that
// answers it, is platform/esp32_t5s3/adp_input.c.
//
// Single-threaded. Someone calls gt911_input_signal() when the controller's
// interrupt fires -- on the board a task the ISR wakes, on the host a test --
// and gt911_input_service() soon after, in a context that may wait on the
// bus. service() reads a report, turns it into events (gt911_touch.h) in
// panel coordinates (the board's swap and mirrors, board_config.h), stamps
// them with port_clock's time of the read, and queues them for poll().
//
// Two things it does without an interrupt, because an edge can be missed --
// the host busy, the line glitching -- and a finger must never stay down for
// ever:
//
//   * while a finger is down, it reads every GT911_INPUT_POLL_WHILE_DOWN_MS
//     whether signalled or not; service() returns when it next wants to be
//     called.
//   * a finger whose reports stop for GT911_INPUT_LIFT_TIMEOUT_MS is lifted.
//     While touched, the controller reports in every scan cycle (7-10 ms in
//     normal mode, its datasheet 8.1 a); silence that long means the report
//     of the lift itself was lost.
//
// And it finds the controller again. A read that fails means the
// controller is gone from its address -- on the board a power glitch the
// ESP32 never saw reset it, and it latched the other address (2026-10-20,
// docs/hardware/board_notes.md, "The touch panel on the board"). Whatever was
// down is lifted, and gt911_init() looks for it at both addresses at once;
// if it is nowhere, again every GT911_INPUT_RETRY_INIT_MS, as after an init
// that found nothing.

#ifndef HAC_DRIVERS_GT911_GT911_INPUT_H
#define HAC_DRIVERS_GT911_GT911_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gt911.h"
#include "gt911_touch.h"
#include "hal_status.h"
#include "port_clock.h"
#include "port_input.h"
#include "port_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// CHOICE: events waiting for the consumer. A tap is two; the event loop may
// be inside a 3.7 s full refresh, during which a finger dragged across the
// glass reports a move every GT911_INPUT_MOVE_THRESHOLD_PX. Past this the
// newest are dropped, and counted.
#define GT911_INPUT_QUEUE_LEN 32u

// CHOICE: how far a finger must go from where it was last reported before a
// move is: about a millimetre on this panel (960 px across 104 mm of glass),
// below the wobble of a finger held still.
#define GT911_INPUT_MOVE_THRESHOLD_PX 8u

// CHOICE: while a finger is down, read at least this often, interrupt or
// not -- five scan cycles.
#define GT911_INPUT_POLL_WHILE_DOWN_MS 50u

// ESTIMATE: no report for this long with a finger down, and it has lifted.
// Twenty-five scan cycles of 10 ms -- the datasheet's slowest normal-mode
// cycle; the green mode's 40 ms cycle (8.1 b) begins only after seconds with
// no touch at all. The board's first taps say whether a held finger ever
// goes this long unreported (`touch` at the console counts timed-out lifts).
#define GT911_INPUT_LIFT_TIMEOUT_MS 250u

// CHOICE: how often a controller that is not there is looked for again --
// after an init that found nothing, or one lost and not found at once. Each
// look is a second of address-only probes, which cost no error lines, and
// one "not found" line in the log: a minute keeps that quiet, and finds a
// controller back within a minute.
#define GT911_INPUT_RETRY_INIT_MS 60000u

// The last events, raw and mapped, for the console.
#define GT911_INPUT_HISTORY 8u

#define GT911_INPUT_NO_DEADLINE UINT64_MAX

typedef struct {
    gt911_config_t device;
    gt911_touch_map_t map;
    uint16_t move_threshold_px;
    const port_log_t *log;  // may be NULL
} gt911_input_config_t;

// One event as the console shows it: where the controller put it, and where
// that is on the panel.
typedef struct {
    input_kind_t kind;
    uint8_t id;
    uint16_t raw_x;
    uint16_t raw_y;
    uint16_t x;
    uint16_t y;
    uint64_t ts_ms;
} gt911_input_record_t;

typedef struct {
    uint32_t signals;      // gt911_input_signal() calls
    uint32_t reads;        // reports asked for
    uint32_t reports;      // reads that found one
    uint32_t empty_reads;  // reads that found none
    uint32_t read_errors;  // reads that failed
    uint32_t events;       // queued
    uint32_t dropped;      // not queued: the queue was full
    uint32_t timed_out_lifts;
    uint32_t losses;         // reads that failed and set off a search
    uint32_t init_attempts;  // gt911_init() calls, the first included
    uint32_t refound;        // searches after a loss that found it
    hal_status_t last_error;
} gt911_input_stats_t;

typedef struct {
    port_input_t port;
    gt911_t dev;
    gt911_touch_map_t map;
    uint16_t move_threshold_px;
    const port_clock_t *clock;
    const port_log_t *log;

    bool ready;  // the controller is found and answering
    bool tried;  // init() was called: a controller not ready is looked for again
    uint64_t next_init_ms;
    bool signalled;
    gt911_touch_t touch;
    uint16_t raw_x[GT911_TOUCH_MAX_POINTS];  // each place's last raw position
    uint16_t raw_y[GT911_TOUCH_MAX_POINTS];
    uint64_t last_report_ms;
    uint64_t next_read_ms;

    input_event_t queue[GT911_INPUT_QUEUE_LEN];
    size_t head;
    size_t count;

    gt911_input_record_t history[GT911_INPUT_HISTORY];
    size_t history_next;
    size_t history_count;

    gt911_input_stats_t stats;
} gt911_input_t;

// Copies `cfg`; no bus traffic. HAL_ERR_INVALID_ARG as gt911_setup(), or for
// a map with no panel size.
hal_status_t gt911_input_setup(gt911_input_t *in, const gt911_input_config_t *cfg);

// gt911_init(): wakes and finds the controller. poll() returns nothing until
// this has succeeded; if it fails, service() tries again every
// GT911_INPUT_RETRY_INIT_MS.
hal_status_t gt911_input_init(gt911_input_t *in);

// True while the controller is found and answering.
bool gt911_input_ready(const gt911_input_t *in);

// The controller's interrupt fired: the next service() reads.
void gt911_input_signal(gt911_input_t *in);

// Reads when signalled or due, queues what the report means, lifts a finger
// whose reports stopped, looks for a controller that failed a read or was
// not found. Returns when it next wants to be called without an interrupt --
// GT911_INPUT_NO_DEADLINE while no finger is down and the controller is
// there, or before init() was ever called.
uint64_t gt911_input_service(gt911_input_t *in);

port_input_t *gt911_input_port(gt911_input_t *in);

// gt911_sleep() / gt911_wake(). A finger down when the controller goes to
// sleep is lifted, now: nothing will report it again.
hal_status_t gt911_input_sleep(gt911_input_t *in);
hal_status_t gt911_input_wake(gt911_input_t *in);

const gt911_t *gt911_input_device(const gt911_input_t *in);
const gt911_input_stats_t *gt911_input_stats(const gt911_input_t *in);

// Up to `cap` of the last GT911_INPUT_HISTORY events, oldest first.
size_t gt911_input_history(const gt911_input_t *in, gt911_input_record_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif  // HAC_DRIVERS_GT911_GT911_INPUT_H
