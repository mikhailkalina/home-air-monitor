// SPDX-License-Identifier: Apache-2.0
//
// The e-paper behaviour both host display adapters share: how long a flush
// takes, when a partial refresh has to be turned into a full one, and the log
// line that says what happened.
//
// It lives outside adp_display_sdl.c on purpose. The window makes a bad
// refresh strategy noticeable; the log is what makes it diagnosable, and CI
// has no window. Running headless must therefore produce the same accounting
// and the same forced full refreshes as running with a window -- otherwise
// "it passed in CI" would mean nothing about the panel.
//
// Every constant it uses comes from epd_emulation.h, measured on the panel in
// phase 2c; see that file for what each one is and how far it is from the
// glass.

#ifndef HAC_PLATFORM_HOST_EPD_MODEL_H
#define HAC_PLATFORM_HOST_EPD_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "port_display.h"
#include "ui/framebuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

// GRAY4 white: what the panel shows after a clear, and at power-up.
#define EPD_MODEL_GRAY4_WHITE 0x0Fu

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t max_partial_refreshes_before_full;

    uint32_t partials_since_full;
    uint32_t flush_count;
    uint32_t forced_full_count;  // how often the panel had to rescue the policy
    uint64_t simulated_ms_total;
} epd_model_t;

typedef struct {
    rect_t area;
    refresh_mode_t requested;
    refresh_mode_t performed;  // differs from `requested` when forced_full
    bool forced_full;
    uint32_t changed_rows;  // rows holding a pixel the refresh changes
    uint32_t written_rows;  // rows each pass writes out (epd_model.c, written_rows())
    uint32_t duration_ms;
    uint32_t area_percent;         // of the whole panel, rounded
    uint32_t partials_since_full;  // after this flush
} epd_flush_t;

// `max_partial_refreshes_before_full` is the budget the panel advertises
// through port_display; 0 means no usable partial mode, and every flush comes
// out full.
void epd_model_init(epd_model_t *m, uint16_t width, uint16_t height,
                    uint32_t max_partial_refreshes_before_full);

// Clips `area` (NULL means the whole panel) to the panel bounds.
rect_t epd_model_clip(const epd_model_t *m, const rect_t *area);

// Accounts for one flush and reports what the panel actually did. Performs no
// drawing and no waiting: the caller applies out->duration_ms itself, because
// how long to really sleep depends on the simulator's time scale.
//
// The duration depends on the rows each pass writes, counted the way EPDiy
// walks them (epd_model.c, written_rows()): it changes the rows of `area` in
// which some pixel of `next` differs from `glass` for a partial or fast-mono
// refresh, and the rows of `next` that are not entirely white for a full one,
// since the clear leaves EPDiy's model white and GC16 drives only what differs
// from it. A full refresh and a clear always cover the whole panel, as on the
// device, whatever `area` says.
// `next` is the framebuffer being flushed and `glass` what the panel shows now,
// one GRAY4 level per pixel, row-major, width x height. Either one NULL counts
// every row of the area -- the worst case, which is also what a rectangle whose
// every row changed costs.
void epd_model_flush(epd_model_t *m, const rect_t *area, refresh_mode_t mode,
                     const framebuffer_t *next, const uint8_t *glass, epd_flush_t *out);

// One line per flush on stdout, plus a second, explicit line whenever the
// panel had to force a full refresh. This is the artifact a bad refresh
// strategy shows up in.
void epd_model_log(const epd_model_t *m, const epd_flush_t *flush);

// End-of-run accounting: flushes, how much of the run was spent driving the
// panel, and -- the number that matters -- how often the panel had to force a
// full refresh because the refresh strategy did not.
void epd_model_log_summary(const epd_model_t *m);

const char *epd_refresh_mode_name(refresh_mode_t mode);

// A full refresh's duration split into what the glass shows in turn: the
// clear's flashing (clear_ms), EPDiy's difference pass, during which nothing
// changes (diff_ms), GC16's dark passes, when every pixel it drives goes black
// (dark_ms), and its light passes, when the picture appears (light_ms). The
// four add up to flush->duration_ms. A clear is all clear_ms; any other
// refresh is all light_ms.
typedef struct {
    uint32_t clear_ms;
    uint32_t diff_ms;
    uint32_t dark_ms;
    uint32_t light_ms;
} epd_full_phases_t;

epd_full_phases_t epd_model_full_phases(const epd_flush_t *flush);

// The pixels GC16 drives after a clear, as masks over `area` of `next`: rows[y]
// is 1 where row y holds a non-white pixel, cols[x] where column x does. A
// pixel is driven -- and goes black in the dark passes -- where both are 1.
// `rows` has `next->height` entries and `cols` `next->width`; entries outside
// `area` are set to 0.
void epd_model_gc16_masks(const framebuffer_t *next, const rect_t *area, uint8_t *rows,
                          uint8_t *cols);

// The 8-bit sRGB grey a GRAY4 level looks like on the glass: its measured
// lightness (EPD_SIM_LEVEL_LIGHTNESS_PERMILLE, epd_emulation.h) encoded for a
// screen, with the panel's black shown as the screen's black and its white as
// the screen's white. The panel's own contrast -- its black is a dark grey --
// is not modelled. A level above 15 counts as 15.
uint8_t epd_level_display_grey(uint8_t level);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_EPD_MODEL_H
