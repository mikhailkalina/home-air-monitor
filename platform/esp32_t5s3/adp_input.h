// SPDX-License-Identifier: Apache-2.0
//
// port_input on the board: the GT911 behind its interrupt
// (docs/adr/0025-touch-input-gt911.md). Everything that decides anything --
// reading the controller, what a report means, when a silent finger has
// lifted, the coordinate mapping -- is drivers/gt911/gt911_input.c, which
// the host tests against the GT911 model. This adds only what is the
// board's:
//
//   * the INT line: BOARD_TOUCH_IRQ_GPIO as the interrupt input, and as the
//     output gt911.h drives to put the controller to sleep and wake it, at
//     BOARD_TOUCH_INT_WAKE_DRIVE;
//   * the interrupt, on the edge the vendor's configuration selects
//     (Module_Switch1, read at init). The ISR only signals a task;
//   * that task, which reads the controller over the shared bus -- adp_i2c's
//     mutex shares it with the sensors -- stamps the events with port_clock's
//     time of the read, and queues them here. The event loop may be inside a
//     3.7 s full refresh when a finger lands; the events wait in this queue,
//     with the time they happened, and the task wakes the loop through the
//     waker the composition gives it (the event loop's own queue);
//   * a mutex between that task and the console's touch_sleep and
//     touch_wake, which drive the same controller.
//
// The controller is found by that task, not by the caller of
// adp_input_init(): looking for one that does not answer at once takes up to
// a second, which the first frame should not wait for -- and on the board it
// once took app_main's 3.5 KB stack over the edge (2026-10-20): the
// controller had latched its other address after a power glitch, and a
// second of failed reads at the first, each logged three times by ESP-IDF's
// I2C driver on the caller's stack, overflowed it into the heap. The driver
// now looks by address-only probes, which log nothing (gt911.h), and the task
// has the stack for what is left.
//
// GPIO47 is not one of the S3's RTC GPIOs (0-21): it cannot wake the chip
// from deep sleep, only from light sleep -- the vendor's example says as much
// -- which phase 5 inherits (docs/hardware/board_notes.md).

#ifndef HAC_PLATFORM_ESP32_T5S3_ADP_INPUT_H
#define HAC_PLATFORM_ESP32_T5S3_ADP_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gt911_input.h"
#include "hal_status.h"
#include "port_clock.h"
#include "port_i2c.h"
#include "port_input.h"
#include "port_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// CHOICE: events between the touch task and the event loop. A full refresh
// is 3.7 s; a finger dragged through one reports a move every
// GT911_INPUT_MOVE_THRESHOLD_PX, so this holds a slow drag across the panel.
// Past it, the newest are dropped and counted.
#define ADP_INPUT_QUEUE_LEN 32u

// How often adp_input_wait_ready() looks.
#define ADP_INPUT_WAIT_POLL_MS 10u

// Called from the touch task when it has queued events.
typedef void (*adp_input_waker_fn)(void *ctx);

typedef struct {
    port_i2c_t *bus;
    const port_clock_t *clock;
    const port_log_t *log;
    uint32_t task_stack_bytes;
    int task_priority;
    int task_core;
} adp_input_config_t;

typedef struct {
    port_input_t port;
    gt911_input_t gt;
    gt911_int_line_t line;
    adp_input_config_t cfg;

    void *queue;  // QueueHandle_t of input_event_t
    void *lock;   // SemaphoreHandle_t: the controller, the touch task against the console
    void *task;   // TaskHandle_t

    adp_input_waker_fn waker;
    void *waker_ctx;

    bool isr_on;  // the interrupt is installed and enabled
    bool level_triggered;
    volatile bool init_done;  // the task's first init has run
    volatile uint32_t interrupts;
    uint32_t queue_dropped;
    uint32_t stack_free_min_bytes;
    hal_status_t init_status;
} adp_input_t;

// Brings the INT line up as an input and starts the touch task, which wakes
// and finds the controller (gt911_input_init()) and then turns the interrupt
// on. Returns at once; HAL_OK means the task runs. Call it once the I2C bus
// is up. Without a controller poll() returns nothing, the task looks for one
// every GT911_INPUT_RETRY_INIT_MS, and the rest of the firmware runs.
hal_status_t adp_input_init(adp_input_t *a, const adp_input_config_t *cfg);

// Waits up to `timeout_ms` for the task's first init: its status, or
// HAL_ERR_TIMEOUT. For the test app; the firmware does not wait.
hal_status_t adp_input_wait_ready(adp_input_t *a, uint32_t timeout_ms);

port_input_t *adp_input_port(adp_input_t *a);

// Who the touch task wakes when it has queued events. May be set at any time.
void adp_input_set_waker(adp_input_t *a, adp_input_waker_fn fn, void *ctx);

// gt911_input_sleep() / gt911_input_wake(), under the lock: for the console,
// and phase 5.
hal_status_t adp_input_sleep(adp_input_t *a);
hal_status_t adp_input_wake(adp_input_t *a);

// What the console's `touch` prints.
typedef struct {
    hal_status_t init_status;
    bool init_done;
    bool found;
    bool asleep;
    gt911_info_t info;
    gt911_input_stats_t stats;
    uint32_t interrupts;
    uint32_t queue_dropped;
    uint32_t stack_free_min_bytes;
    gt911_input_record_t history[GT911_INPUT_HISTORY];
    size_t history_count;
} adp_input_status_t;

void adp_input_status(adp_input_t *a, adp_input_status_t *out);

// The interrupt off, the task stopped, the queue gone: for the test app,
// whose cases each bring their own bus up and down. The controller is left
// as it is, awake or asleep.
void adp_input_deinit(adp_input_t *a);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_ADP_INPUT_H
