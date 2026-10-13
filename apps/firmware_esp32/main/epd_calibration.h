// SPDX-License-Identifier: Apache-2.0
//
// The panel calibration mode of phase 2c (CONFIG_HAC_EPD_CALIBRATION, off by
// default -- see main/Kconfig.projbuild for what it costs and why it is kept).
//
// It exists to replace the estimates in platform/host/epd_emulation.h and the
// guesses in platform/esp32_t5s3/board_config.h with numbers taken from this
// panel, through the same port_display the product uses. Everything is timed
// around port_display->flush() rather than around EPDiy's own calls, because
// flush() is what the core waits on and what the simulator emulates: the
// power bracket, the epd_clear() a full refresh starts with and the adapter's
// own logging are all part of the latency the user sees.
//
// In this order:
//
//   0. a baseline 16-level grey ramp, showing residue earlier runs left;
//   1. a ghosting probe, which needs someone watching the panel (it starts
//      early, while whoever flashed the board is still looking at it);
//   2. timing of every refresh_mode_t, unattended, ending in one "result"
//      log line per case with min, max and mean;
//   2a. optionally, a residue test: does what partial refreshes leave behind
//      survive ordinary full refreshes, and does a deeper clean remove it;
//   3. a closing grey ramp, left on the panel for a visual check of the
//      waveform's greys on this unit and of the residue this run added.
//
// It never returns early on a failed flush: a failure is counted against its
// case and reported in the summary, so one bad mode does not hide the rest.

#ifndef HAC_APPS_FIRMWARE_ESP32_EPD_CALIBRATION_H
#define HAC_APPS_FIRMWARE_ESP32_EPD_CALIBRATION_H

#include <stdint.h>

#include "port_clock.h"
#include "port_display.h"
#include "port_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// What the ghosting probe does to the CO2 headline between steps
// (epd_calibration.c explains each).
typedef enum {
    EPD_CAL_GHOST_SAWTOOTH = 0,
    EPD_CAL_GHOST_SHIFT,
} epd_calibration_ghost_pattern_t;

typedef struct {
    // Per timing case: one sample per iteration for whole-panel cases, two
    // (there and back) for partial and fast-mono ones. 0 skips the timing.
    uint32_t iterations;

    epd_calibration_ghost_pattern_t ghost_pattern;

    // Partial refreshes in the ghosting probe; 0 skips the probe.
    uint32_t ghost_partials;

    // Pause after each probe step, for the observer.
    uint32_t ghost_pause_ms;

    // Residue test (epd_calibration.c): ordinary full refreshes, each shown
    // on a ramp, then deep events of `residue_extra_clears` REFRESH_CLEARs
    // back to back before the ramp's own full refresh. Zeroes skip it.
    uint32_t residue_normal_fulls;
    uint32_t residue_deep_events;
    uint32_t residue_extra_clears;
} epd_calibration_config_t;

// Runs every part against `display`, blocking for as long as they take
// (roughly 20 minutes at the Kconfig defaults). Whole-panel drives honour
// display->min_full_refresh_interval_ms and the timing batches stay within
// display->max_partial_refreshes_before_full; only the ghosting probe exceeds
// the budget, deliberately and bounded by cfg->ghost_partials.
//
// It runs on app_main's own stack. Keeping its working state static is not
// enough on its own: the call chain through flush() and ESP_LOG needs more
// than the default main-task stack, so a calibration build must raise it
// (sdkconfig.calibration does; epd_calibration.c refuses to compile
// otherwise, and says why).
void epd_calibration_run(port_display_t *display, const port_clock_t *clock, const port_log_t *log,
                         const epd_calibration_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif  // HAC_APPS_FIRMWARE_ESP32_EPD_CALIBRATION_H
