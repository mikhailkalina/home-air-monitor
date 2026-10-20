// SPDX-License-Identifier: Apache-2.0

#include "gt911_touch.h"

#include <string.h>

// Track IDs the ignored-finger set can hold; a GT911's are below 16 in
// practice, and one above is simply never followed.
#define IGNORED_IDS 32u

// The most points one snapshot is looked at for: a GT911 reports five.
#define UNIQUE_MAX 8u

void gt911_touch_init(gt911_touch_t *t)
{
    memset(t, 0, sizeof(*t));
}

bool gt911_touch_any_down(const gt911_touch_t *t)
{
    for (size_t i = 0u; i < GT911_TOUCH_MAX_POINTS; ++i) {
        if (t->places[i].down) {
            return true;
        }
    }
    return false;
}

static const gt911_touch_point_t *find_point(const gt911_touch_point_t *points, size_t count,
                                             uint8_t track_id)
{
    for (size_t i = 0u; i < count; ++i) {
        if (points[i].track_id == track_id) {
            return &points[i];
        }
    }
    return NULL;
}

static int find_place(const gt911_touch_t *t, uint8_t track_id)
{
    for (size_t i = 0u; i < GT911_TOUCH_MAX_POINTS; ++i) {
        if (t->places[i].down && t->places[i].track_id == track_id) {
            return (int)i;
        }
    }
    return -1;
}

static uint16_t distance(uint16_t a, uint16_t b)
{
    return (uint16_t)((a > b) ? a - b : b - a);
}

static void emit(input_event_t *out, size_t cap, size_t *n, input_kind_t kind, size_t place,
                 uint16_t x, uint16_t y, uint64_t ts_ms)
{
    if (*n >= cap) {
        return;
    }
    input_event_t *ev = &out[*n];
    memset(ev, 0, sizeof(*ev));
    ev->kind = kind;
    ev->x = x;
    ev->y = y;
    ev->id = (uint8_t)place;
    ev->ts_ms = ts_ms;
    (*n)++;
}

size_t gt911_touch_update(gt911_touch_t *t, const gt911_touch_point_t *points, size_t count,
                          uint16_t move_threshold_px, uint64_t ts_ms, input_event_t *out,
                          size_t cap)
{
    size_t n = 0u;

    // A track ID reported twice in one snapshot is the controller's error;
    // only its first record counts.
    gt911_touch_point_t unique[UNIQUE_MAX];
    size_t unique_count = 0u;
    for (size_t i = 0u; i < count && unique_count < sizeof(unique) / sizeof(unique[0]); ++i) {
        if (find_point(unique, unique_count, points[i].track_id) == NULL) {
            unique[unique_count++] = points[i];
        }
    }

    // Lifted: every UP first, so a place freed here can be taken below.
    for (size_t p = 0u; p < GT911_TOUCH_MAX_POINTS; ++p) {
        gt911_touch_place_t *place = &t->places[p];
        if (place->down && find_point(unique, unique_count, place->track_id) == NULL) {
            emit(out, cap, &n, INPUT_TOUCH_UP, p, place->x, place->y, ts_ms);
            place->down = false;
        }
    }

    // Still down: a move, once it has gone far enough from its last report.
    for (size_t p = 0u; p < GT911_TOUCH_MAX_POINTS; ++p) {
        gt911_touch_place_t *place = &t->places[p];
        if (!place->down) {
            continue;
        }
        const gt911_touch_point_t *pt = find_point(unique, unique_count, place->track_id);
        if (distance(pt->x, place->x) > move_threshold_px ||
            distance(pt->y, place->y) > move_threshold_px) {
            place->x = pt->x;
            place->y = pt->y;
            emit(out, cap, &n, INPUT_TOUCH_MOVE, p, pt->x, pt->y, ts_ms);
        }
    }

    // Fingers no longer reported are no longer ignored.
    uint32_t still_ignored = 0u;
    for (size_t i = 0u; i < unique_count; ++i) {
        const uint8_t id = unique[i].track_id;
        if (id < IGNORED_IDS && (t->ignored & (1u << id)) != 0u) {
            still_ignored |= 1u << id;
        }
    }
    t->ignored = still_ignored;

    // New: the lowest free place, in the order the controller reported them;
    // past the last place, ignored for as long as they stay down.
    for (size_t i = 0u; i < unique_count; ++i) {
        const gt911_touch_point_t *pt = &unique[i];
        if (find_place(t, pt->track_id) >= 0) {
            continue;
        }
        if (pt->track_id >= IGNORED_IDS || (t->ignored & (1u << pt->track_id)) != 0u) {
            continue;
        }
        size_t p = 0u;
        while (p < GT911_TOUCH_MAX_POINTS && t->places[p].down) {
            ++p;
        }
        if (p == GT911_TOUCH_MAX_POINTS) {
            t->ignored |= 1u << pt->track_id;
            continue;
        }
        t->places[p].down = true;
        t->places[p].track_id = pt->track_id;
        t->places[p].x = pt->x;
        t->places[p].y = pt->y;
        emit(out, cap, &n, INPUT_TOUCH_DOWN, p, pt->x, pt->y, ts_ms);
    }
    return n;
}

static uint16_t scale(uint16_t v, uint16_t v_max, uint16_t n)
{
    if (n == 0u) {
        return 0u;
    }
    uint32_t s = v;
    if (v_max != 0u && v_max != n) {
        s = (uint32_t)v * n / v_max;
    }
    return (uint16_t)((s >= n) ? (uint32_t)n - 1u : s);
}

void gt911_touch_map_point(const gt911_touch_map_t *map, uint16_t x_max, uint16_t y_max, uint16_t x,
                           uint16_t y, uint16_t *panel_x, uint16_t *panel_y)
{
    uint16_t a = x;
    uint16_t b = y;
    uint16_t a_max = x_max;
    uint16_t b_max = y_max;
    if (map->swap_xy) {
        a = y;
        b = x;
        a_max = y_max;
        b_max = x_max;
    }
    uint16_t px = scale(a, a_max, map->panel_width);
    uint16_t py = scale(b, b_max, map->panel_height);
    if (map->mirror_x && map->panel_width > 0u) {
        px = (uint16_t)(map->panel_width - 1u - px);
    }
    if (map->mirror_y && map->panel_height > 0u) {
        py = (uint16_t)(map->panel_height - 1u - py);
    }
    *panel_x = px;
    *panel_y = py;
}

const char *gt911_touch_kind_name(input_kind_t kind)
{
    switch (kind) {
        case INPUT_TOUCH_DOWN:
            return "down";
        case INPUT_TOUCH_UP:
            return "up";
        case INPUT_TOUCH_MOVE:
            return "move";
        case INPUT_BUTTON:
            return "button";
    }
    return "?";
}
