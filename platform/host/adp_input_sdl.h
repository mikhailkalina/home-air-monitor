// SPDX-License-Identifier: Apache-2.0
//
// Mouse to touch. A press becomes INPUT_TOUCH_DOWN at the panel coordinate
// under the cursor, a release INPUT_TOUCH_UP, and motion INPUT_TOUCH_MOVE but
// only while a button is held -- a touch panel has no hover, and letting the
// core see one would be a behaviour the hardware can never produce.
//
// Coordinates are divided by the window zoom before they leave here, so the
// core always works in panel pixels regardless of how the window is scaled.
//
// This adapter also owns SDL's event queue, because there is only one and it
// has to be drained somewhere: closing the window or pressing Escape is
// recorded here and read back with adp_input_sdl_quit_requested().

#ifndef HAC_PLATFORM_HOST_ADP_INPUT_SDL_H
#define HAC_PLATFORM_HOST_ADP_INPUT_SDL_H

#include <stdbool.h>
#include <stdint.h>

#include "port_clock.h"
#include "port_input.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    port_input_t port;

    const port_clock_t *clock;  // timestamps come from the simulator's virtual clock
    uint16_t width;
    uint16_t height;
    float zoom;

    bool touching;        // a button is currently down
    bool quit_requested;  // the window was closed, or Escape was pressed
} adp_input_sdl_t;

// `width` and `height` are the panel's, `zoom` the window scale used by
// adp_display_sdl_create(). `clock` must outlive the adapter.
void adp_input_sdl_init(adp_input_sdl_t *in, uint16_t width, uint16_t height, float zoom,
                        const port_clock_t *clock);

port_input_t *adp_input_sdl_port(adp_input_sdl_t *in);

// True once the user has asked for the window to close. Poll the port until
// it returns false first: the flag is set while draining SDL's queue.
bool adp_input_sdl_quit_requested(const adp_input_sdl_t *in);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_ADP_INPUT_SDL_H
