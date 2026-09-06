// SPDX-License-Identifier: Apache-2.0
//
// The windowed display adapter: the panel at 1:1 (or scaled by --zoom) in 16
// greys, with the parts of e-paper behaviour that a still image cannot show --
// refresh latency you wait through, the inversion flash of a full refresh,
// ghosting accumulating across partial refreshes, and an outline around the
// rectangle each flush actually touched.
//
// The flush accounting, the forced full refresh and the log lines are shared
// with the headless adapter through epd_model.h; only the picture is new here.
// All timing and ghosting figures are unmeasured estimates from
// epd_emulation.h.
//
// Built only when SIM_GUI is ON and SDL2 was found. Nothing in core/ knows
// this file exists, and nothing in it knows what a reading is.

#ifndef HAC_PLATFORM_HOST_ADP_DISPLAY_SDL_H
#define HAC_PLATFORM_HOST_ADP_DISPLAY_SDL_H

#include <stdint.h>

#include "epd_model.h"
#include "hal_status.h"
#include "port_display.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct adp_display_sdl_s adp_display_sdl_t;

// Opens a window of width*zoom by height*zoom.
//
// `time_scale` is the simulator's --time-scale: the emulated refresh latency
// is divided by it before the adapter really sleeps, so an accelerated run
// does not spend its life waiting on the panel while the log still reports
// the true panel duration. At scale 1 you wait exactly as long as the
// hardware would make you wait, which is the point of emulating it at all.
//
// Returns NULL if SDL or the window cannot be created; the reason is printed.
adp_display_sdl_t *adp_display_sdl_create(uint16_t width, uint16_t height, float zoom,
                                          double time_scale);

void adp_display_sdl_destroy(adp_display_sdl_t *d);

port_display_t *adp_display_sdl_port(adp_display_sdl_t *d);

// The panel model behind this adapter, for end-of-run accounting.
const epd_model_t *adp_display_sdl_model(const adp_display_sdl_t *d);

// Repaints the window from the current glass contents, without touching the
// panel model. Call once per event-loop iteration so the window stays live
// between flushes.
void adp_display_sdl_present(adp_display_sdl_t *d);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_ADP_DISPLAY_SDL_H
