// SPDX-License-Identifier: Apache-2.0
//
// What counts as a tap: one finger down and up again, soon and close
// (docs/adr/0025-touch-input-gt911.md, "Taps").
//
// Fed the events port_input delivers, in order, it says when one completes a
// tap. Times are the events' own -- port_clock's, taken when the controller
// was read -- not the time they are handed over, so a tap made while the
// event loop sat in a 3.7 s full refresh is still a tap when the loop gets
// to it. Pure: no clock, no I/O.
//
// A tap is spoiled, and nothing is reported for it, by:
//
//   * the finger travelling more than max_travel_px from where it went down,
//     on either axis -- a drag, not a tap;
//   * the finger staying down longer than max_duration_ms -- a press, which
//     a later phase may want for something else;
//   * a second finger down at any time during it.
//
// An up whose down was never seen, and a second down of the finger already
// followed (its up lost), start over rather than guess.

#ifndef HAC_CORE_APP_TAP_H
#define HAC_CORE_APP_TAP_H

#include <stdbool.h>
#include <stdint.h>

#include "port_input.h"

#ifdef __cplusplus
extern "C" {
#endif

// CHOICE: a tap is shorter than this. Taps on a phone are around 100 ms; a
// deliberate one on a panel that answers seconds later is slower. Long
// enough for that, short enough that resting a finger on the glass is not
// one.
#define TAP_DEFAULT_MAX_DURATION_MS 800u

// CHOICE: and travels no further than this -- about 4 mm on this panel (960
// px across 104 mm), five of the touch adapter's move thresholds.
#define TAP_DEFAULT_MAX_TRAVEL_PX 40u

typedef struct {
    uint32_t max_duration_ms;
    uint16_t max_travel_px;
} tap_config_t;

typedef struct {
    tap_config_t cfg;
    uint8_t down_mask;  // the fingers down, by event id (below 8)
    bool tracking;
    bool spoiled;
    uint8_t id;
    uint16_t x;
    uint16_t y;
    uint64_t down_ms;
} tap_t;

// A completed tap: where the finger went down, and when it went down and up.
typedef struct {
    uint16_t x;
    uint16_t y;
    uint64_t down_ms;
    uint64_t up_ms;
} tap_event_t;

tap_config_t tap_config_default(void);

void tap_init(tap_t *t, const tap_config_t *cfg);

// Feeds one event. True, with *out filled, when it completes a tap.
bool tap_feed(tap_t *t, const input_event_t *ev, tap_event_t *out);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_APP_TAP_H
