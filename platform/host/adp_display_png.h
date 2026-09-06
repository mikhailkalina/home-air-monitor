// SPDX-License-Identifier: Apache-2.0
//
// The headless display adapter: every flush becomes a PNG under an output
// directory, and the same flush accounting and log lines as the SDL window
// produces (epd_model.h). This is the path CI runs, and the path the golden
// screenshots come from, so it needs no graphics stack at all.

#ifndef HAC_PLATFORM_HOST_ADP_DISPLAY_PNG_H
#define HAC_PLATFORM_HOST_ADP_DISPLAY_PNG_H

#include <stdint.h>

#include "epd_model.h"
#include "hal_status.h"
#include "port_display.h"
#include "ui/framebuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    port_display_t port;
    framebuffer_t fb;
    uint8_t *pixels;   // the GRAY4 framebuffer the core draws into
    uint8_t *scratch;  // width * height bytes, for the 8-bit conversion
    epd_model_t model;

    char out_dir[512];
    uint32_t frame_index;
    bool write_frames;  // false when no output directory was given
} adp_display_png_t;

// Allocates the framebuffer and creates `out_dir` if it does not exist.
// `out_dir` may be NULL, in which case flushes are accounted for and logged
// but no files are written -- useful for a scenario run that only cares about
// the refresh sequence.
//
// Returns HAL_ERR_NO_MEM if the buffers cannot be allocated, HAL_ERR_IO if the
// directory cannot be created.
hal_status_t adp_display_png_init(adp_display_png_t *d, uint16_t width, uint16_t height,
                                  const char *out_dir);

port_display_t *adp_display_png_port(adp_display_png_t *d);

void adp_display_png_deinit(adp_display_png_t *d);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_ADP_DISPLAY_PNG_H
