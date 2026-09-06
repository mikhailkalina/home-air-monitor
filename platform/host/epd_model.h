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
// Every constant it uses comes from epd_emulation.h, and every one of those is
// an unmeasured estimate; see that file.

#ifndef HAC_PLATFORM_HOST_EPD_MODEL_H
#define HAC_PLATFORM_HOST_EPD_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "port_display.h"

#ifdef __cplusplus
extern "C" {
#endif

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
void epd_model_flush(epd_model_t *m, const rect_t *area, refresh_mode_t mode, epd_flush_t *out);

// One line per flush on stdout, plus a second, explicit line whenever the
// panel had to force a full refresh. This is the artifact a bad refresh
// strategy shows up in.
void epd_model_log(const epd_model_t *m, const epd_flush_t *flush);

// End-of-run accounting: flushes, how much of the run was spent driving the
// panel, and -- the number that matters -- how often the panel had to force a
// full refresh because the refresh strategy did not.
void epd_model_log_summary(const epd_model_t *m);

const char *epd_refresh_mode_name(refresh_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_EPD_MODEL_H
