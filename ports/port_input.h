// SPDX-License-Identifier: Apache-2.0
//
// The input port: touch panel and buttons, normalized to panel coordinates.
//
// On hardware the adapter drains a GT911 interrupt queue; in the simulator it
// translates mouse events (docs/architecture.md 5.6, 7.2). Either way the core
// sees the same non-blocking poll, and never learns which one it is talking to.

#ifndef HAC_PORTS_PORT_INPUT_H
#define HAC_PORTS_PORT_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INPUT_TOUCH_DOWN,
    INPUT_TOUCH_UP,
    INPUT_TOUCH_MOVE,
    INPUT_BUTTON,
} input_kind_t;

typedef struct {
    input_kind_t kind;
    uint16_t x;  // panel coordinates, already unscaled by any window zoom
    uint16_t y;
    uint8_t id;      // touch point index; 0 for the first finger and for buttons
    uint64_t ts_ms;  // from port_clock, so replayed input keeps its timing
} input_event_t;

typedef struct port_input_s port_input_t;

struct port_input_s {
    // Non-blocking: fills *out and returns true when an event was waiting,
    // returns false when the queue is empty. Call it in a loop until it
    // returns false.
    bool (*poll)(port_input_t *self, input_event_t *out);

    void *impl;
};

#ifdef __cplusplus
}
#endif

#endif  // HAC_PORTS_PORT_INPUT_H
