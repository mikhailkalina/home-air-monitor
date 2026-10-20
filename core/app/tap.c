// SPDX-License-Identifier: Apache-2.0

#include "app/tap.h"

#include <string.h>

// Event ids the finger mask can hold; port_input's are touch places, 0 and 1.
#define MASK_IDS 8u

tap_config_t tap_config_default(void)
{
    tap_config_t cfg;
    cfg.max_duration_ms = TAP_DEFAULT_MAX_DURATION_MS;
    cfg.max_travel_px = TAP_DEFAULT_MAX_TRAVEL_PX;
    return cfg;
}

void tap_init(tap_t *t, const tap_config_t *cfg)
{
    memset(t, 0, sizeof(*t));
    t->cfg = *cfg;
}

static uint16_t distance(uint16_t a, uint16_t b)
{
    return (uint16_t)((a > b) ? a - b : b - a);
}

static bool too_far(const tap_t *t, uint16_t x, uint16_t y)
{
    return distance(x, t->x) > t->cfg.max_travel_px || distance(y, t->y) > t->cfg.max_travel_px;
}

static uint8_t bit(uint8_t id)
{
    return (uint8_t)((id < MASK_IDS) ? (1u << id) : 0u);
}

bool tap_feed(tap_t *t, const input_event_t *ev, tap_event_t *out)
{
    switch (ev->kind) {
        case INPUT_TOUCH_DOWN: {
            const bool others_down = (t->down_mask & (uint8_t)~bit(ev->id)) != 0u;
            t->down_mask |= bit(ev->id);
            if (t->tracking && ev->id != t->id) {
                t->spoiled = true;  // a second finger: not a tap
                return false;
            }
            // A first finger, or the followed one again with its up lost.
            t->tracking = true;
            t->spoiled = others_down;
            t->id = ev->id;
            t->x = ev->x;
            t->y = ev->y;
            t->down_ms = ev->ts_ms;
            return false;
        }
        case INPUT_TOUCH_MOVE:
            if (t->tracking && ev->id == t->id && too_far(t, ev->x, ev->y)) {
                t->spoiled = true;
            }
            return false;
        case INPUT_TOUCH_UP: {
            t->down_mask &= (uint8_t)~bit(ev->id);
            if (!t->tracking || ev->id != t->id) {
                return false;
            }
            t->tracking = false;
            const uint64_t held = (ev->ts_ms >= t->down_ms) ? ev->ts_ms - t->down_ms : 0u;
            if (t->spoiled || held > t->cfg.max_duration_ms || too_far(t, ev->x, ev->y)) {
                return false;
            }
            out->x = t->x;
            out->y = t->y;
            out->down_ms = t->down_ms;
            out->up_ms = ev->ts_ms;
            return true;
        }
        case INPUT_BUTTON:
            break;
    }
    return false;
}
