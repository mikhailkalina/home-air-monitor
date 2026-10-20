// SPDX-License-Identifier: Apache-2.0

#include "adp_input_script.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS_PER_S 1000.0

static bool script_poll(port_input_t *port, input_event_t *out)
{
    adp_input_script_t *s = (adp_input_script_t *)port->impl;
    if (s->next >= s->count) {
        return false;
    }
    const uint64_t now = s->clock->now_ms(s->clock);
    if (s->events[s->next].ts_ms > now) {
        return false;
    }
    *out = s->events[s->next++];
    return true;
}

void adp_input_script_init(adp_input_script_t *s, const port_clock_t *clock)
{
    memset(s, 0, sizeof(*s));
    s->clock = clock;
    s->port.poll = script_poll;
    s->port.impl = s;
}

port_input_t *adp_input_script_port(adp_input_script_t *s)
{
    return &s->port;
}

bool adp_input_script_push(adp_input_script_t *s, input_kind_t kind, uint16_t x, uint16_t y,
                           uint64_t at_ms)
{
    if (s->count >= ADP_INPUT_SCRIPT_MAX_EVENTS ||
        (s->count > 0u && at_ms < s->events[s->count - 1u].ts_ms)) {
        return false;
    }
    input_event_t *ev = &s->events[s->count++];
    memset(ev, 0, sizeof(*ev));
    ev->kind = kind;
    ev->x = x;
    ev->y = y;
    ev->id = 0u;  // one finger
    ev->ts_ms = at_ms;
    return true;
}

uint64_t adp_input_script_next_ms(const adp_input_script_t *s)
{
    return (s->next < s->count) ? s->events[s->next].ts_ms : ADP_INPUT_SCRIPT_NO_EVENT;
}

typedef struct {
    uint64_t at_ms;
    uint16_t x;
    uint16_t y;
} tap_item_t;

// One "<t_s>:<x>,<y>" from `p` up to `end`.
static bool parse_item(const char *p, const char *end, uint16_t width, uint16_t height,
                       tap_item_t *out, char *err, size_t err_cap)
{
    char item[64];
    const size_t len = (size_t)(end - p);
    if (len == 0u || len >= sizeof(item)) {
        snprintf(err, err_cap, "a tap is <seconds>:<x>,<y>, not \"%.*s\"", (int)len, p);
        return false;
    }
    memcpy(item, p, len);
    item[len] = '\0';

    char *rest = NULL;
    const double t_s = strtod(item, &rest);
    if (rest == item || *rest != ':' || !isfinite(t_s) || t_s < 0.0) {
        snprintf(err, err_cap, "a tap is <seconds>:<x>,<y>, not \"%s\"", item);
        return false;
    }
    char *xs = rest + 1;
    const long x = strtol(xs, &rest, 10);
    if (rest == xs || *rest != ',') {
        snprintf(err, err_cap, "a tap is <seconds>:<x>,<y>, not \"%s\"", item);
        return false;
    }
    char *ys = rest + 1;
    const long y = strtol(ys, &rest, 10);
    if (rest == ys || *rest != '\0') {
        snprintf(err, err_cap, "a tap is <seconds>:<x>,<y>, not \"%s\"", item);
        return false;
    }
    if (x < 0 || y < 0 || x >= (long)width || y >= (long)height) {
        snprintf(err, err_cap, "the tap \"%s\" is off the %ux%u panel", item, width, height);
        return false;
    }
    out->at_ms = (uint64_t)llround(t_s * MS_PER_S);
    out->x = (uint16_t)x;
    out->y = (uint16_t)y;
    return true;
}

hal_status_t adp_input_script_add_taps(adp_input_script_t *s, const char *text, uint16_t width,
                                       uint16_t height, char *err, size_t err_cap)
{
    tap_item_t taps[ADP_INPUT_SCRIPT_MAX_EVENTS / 2u];
    size_t n = 0u;
    uint64_t free_from = (s->count > 0u) ? s->events[s->count - 1u].ts_ms : 0u;

    const char *p = text;
    while (*p != '\0') {
        const char *end = strchr(p, ';');
        if (end == NULL) {
            end = p + strlen(p);
        }
        if (n >= sizeof(taps) / sizeof(taps[0]) ||
            s->count + 2u * (n + 1u) > ADP_INPUT_SCRIPT_MAX_EVENTS) {
            snprintf(err, err_cap, "at most %u taps", (unsigned)(ADP_INPUT_SCRIPT_MAX_EVENTS / 2u));
            return HAL_ERR_INVALID_ARG;
        }
        if (!parse_item(p, end, width, height, &taps[n], err, err_cap)) {
            return HAL_ERR_INVALID_ARG;
        }
        if ((n > 0u || s->count > 0u) && taps[n].at_ms < free_from) {
            snprintf(err, err_cap,
                     "the taps must come in time order, each after the last is over (%u ms)",
                     (unsigned)ADP_INPUT_SCRIPT_TAP_MS);
            return HAL_ERR_INVALID_ARG;
        }
        free_from = taps[n].at_ms + ADP_INPUT_SCRIPT_TAP_MS;
        ++n;
        p = (*end == ';') ? end + 1 : end;
    }
    if (n == 0u) {
        snprintf(err, err_cap, "no taps in \"%s\"", text);
        return HAL_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < n; ++i) {
        (void)adp_input_script_push(s, INPUT_TOUCH_DOWN, taps[i].x, taps[i].y, taps[i].at_ms);
        (void)adp_input_script_push(s, INPUT_TOUCH_UP, taps[i].x, taps[i].y,
                                    taps[i].at_ms + ADP_INPUT_SCRIPT_TAP_MS);
    }
    return HAL_OK;
}
