// SPDX-License-Identifier: Apache-2.0
//
// What a touch controller that reports snapshots means as events.
//
// The GT911 says which points are down now, each with a track ID that stays
// with a finger while it stays on the glass (gt911.h). port_input speaks in
// events (ports/port_input.h): a finger went down, moved, came up. This turns
// one into the other, purely -- no bus, no clock -- so its edges are unit
// tests (tests/unit/test_gt911_touch.c):
//
//   * a track ID in the new snapshot and not the old is INPUT_TOUCH_DOWN; in
//     the old and not the new, INPUT_TOUCH_UP, at the last position reported;
//     in both, INPUT_TOUCH_MOVE -- but only once it has moved more than
//     `move_threshold_px` from where it was last reported, on either axis, so
//     a finger held still does not stream moves at the scan rate.
//   * at most GT911_TOUCH_MAX_POINTS points. A finger already followed keeps
//     its place; a new one takes the lowest free one, and beyond that is not
//     seen at all, not even when a place frees later -- it would arrive as a
//     DOWN mid-gesture.
//   * the event's id is that place, 0 or 1, not the controller's track ID:
//     "0 for the first finger", as the port says.
//   * in one snapshot, every UP comes before any DOWN, so a lift and a new
//     finger in the same report free the place before it is taken again.
//
// What a snapshot cannot say: a finger lifted and a new one landed with the
// same track ID between two reports the host saw -- a report lost, or the
// host late -- looks like one finger that jumped. The controller gives
// nothing to tell them apart, and this reports a move.
//
// And the coordinate mapping: the controller's frame -- its own resolution,
// its own axes -- onto the panel's, by the board's swap and mirrors
// (board_config.h). Also pure.

#ifndef HAC_DRIVERS_GT911_GT911_TOUCH_H
#define HAC_DRIVERS_GT911_GT911_TOUCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "port_input.h"

#ifdef __cplusplus
extern "C" {
#endif

// CHOICE: two fingers. The GT911 follows five; nothing this firmware plans
// needs more than two, and port_input's simulator side models two (Shift+click,
// docs/architecture.md 7.2).
#define GT911_TOUCH_MAX_POINTS 2u

// The most events one snapshot can produce: every place lifted and taken.
#define GT911_TOUCH_MAX_EVENTS (2u * GT911_TOUCH_MAX_POINTS)

// One point of a snapshot, already in panel coordinates.
typedef struct {
    uint8_t track_id;
    uint16_t x;
    uint16_t y;
} gt911_touch_point_t;

typedef struct {
    bool down;
    uint8_t track_id;
    uint16_t x;  // where it was last reported
    uint16_t y;
} gt911_touch_place_t;

// The fingers as last reported to the consumer.
typedef struct {
    gt911_touch_place_t places[GT911_TOUCH_MAX_POINTS];
    uint32_t ignored;  // track IDs (below 32) down past the last place: never followed
} gt911_touch_t;

void gt911_touch_init(gt911_touch_t *t);

// Turns the snapshot `points` (`count` of them) into events stamped `ts_ms`,
// written to `out` -- at most `cap`, which should be GT911_TOUCH_MAX_EVENTS --
// and updates `t`. Returns how many were written.
size_t gt911_touch_update(gt911_touch_t *t, const gt911_touch_point_t *points, size_t count,
                          uint16_t move_threshold_px, uint64_t ts_ms, input_event_t *out,
                          size_t cap);

// True while any finger is down.
bool gt911_touch_any_down(const gt911_touch_t *t);

// How the controller's frame lies on the panel's.
typedef struct {
    bool swap_xy;   // the controller's X runs along the panel's Y
    bool mirror_x;  // after the swap: the panel's X runs the other way
    bool mirror_y;
    uint16_t panel_width;
    uint16_t panel_height;
} gt911_touch_map_t;

// (x, y) in a controller frame `x_max` x `y_max` -- its configuration's
// output maximum -- to the panel, clamped inside it: the axes swapped if
// asked, scaled to the panel's size, mirrored if asked. An `x_max` or `y_max`
// of 0 (unknown) takes the panel's own size along that axis.
void gt911_touch_map_point(const gt911_touch_map_t *map, uint16_t x_max, uint16_t y_max, uint16_t x,
                           uint16_t y, uint16_t *panel_x, uint16_t *panel_y);

// "down", "up", "move", "button"; never NULL.
const char *gt911_touch_kind_name(input_kind_t kind);

#ifdef __cplusplus
}
#endif

#endif  // HAC_DRIVERS_GT911_GT911_TOUCH_H
