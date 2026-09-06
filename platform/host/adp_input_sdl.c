// SPDX-License-Identifier: Apache-2.0

#include "adp_input_sdl.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <string.h>

static adp_input_sdl_t *self_of(port_input_t *port)
{
    return (adp_input_sdl_t *)port->impl;
}

// Window pixels to panel pixels, clamped: a drag that leaves the window still
// reports a coordinate on the panel, as a real touch controller would.
static uint16_t to_panel(int window_coord, float zoom, uint16_t limit)
{
    if (window_coord < 0 || limit == 0u) {
        return 0u;
    }
    const float scaled = (float)window_coord / ((zoom > 0.0f) ? zoom : 1.0f);
    if (scaled >= (float)(limit - 1u)) {
        return (uint16_t)(limit - 1u);
    }
    return (uint16_t)scaled;
}

static void fill(adp_input_sdl_t *in, input_event_t *out, input_kind_t kind, int x, int y)
{
    memset(out, 0, sizeof(*out));

    out->kind = kind;
    out->x = to_panel(x, in->zoom, in->width);
    out->y = to_panel(y, in->zoom, in->height);
    out->id = 0u;  // one finger: multi-touch arrives with the GT911 in phase 2
    out->ts_ms = (in->clock != NULL) ? in->clock->now_ms(in->clock) : 0u;
}

static bool sdl_poll(port_input_t *port, input_event_t *out)
{
    adp_input_sdl_t *in = self_of(port);
    SDL_Event ev;

    // Drains SDL's queue until it yields something the core cares about, so
    // window and keyboard events are always consumed even when no touch
    // results. The caller loops until this returns false.
    while (SDL_PollEvent(&ev) != 0) {
        switch (ev.type) {
            case SDL_QUIT:
                in->quit_requested = true;
                break;

            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_CLOSE) {
                    in->quit_requested = true;
                }
                break;

            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_ESCAPE) {
                    in->quit_requested = true;
                }
                break;

            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    in->touching = true;
                    fill(in, out, INPUT_TOUCH_DOWN, ev.button.x, ev.button.y);
                    return true;
                }
                break;

            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT && in->touching) {
                    in->touching = false;
                    fill(in, out, INPUT_TOUCH_UP, ev.button.x, ev.button.y);
                    return true;
                }
                break;

            case SDL_MOUSEMOTION:
                // No hover on a touch panel: motion counts only under a press.
                if (in->touching) {
                    fill(in, out, INPUT_TOUCH_MOVE, ev.motion.x, ev.motion.y);
                    return true;
                }
                break;

            default:
                break;
        }
    }

    return false;
}

void adp_input_sdl_init(adp_input_sdl_t *in, uint16_t width, uint16_t height, float zoom,
                        const port_clock_t *clock)
{
    memset(in, 0, sizeof(*in));

    in->clock = clock;
    in->width = width;
    in->height = height;
    in->zoom = (zoom > 0.0f) ? zoom : 1.0f;

    in->port.poll = sdl_poll;
    in->port.impl = in;
}

port_input_t *adp_input_sdl_port(adp_input_sdl_t *in)
{
    return &in->port;
}

bool adp_input_sdl_quit_requested(const adp_input_sdl_t *in)
{
    return in->quit_requested;
}
