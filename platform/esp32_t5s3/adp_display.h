// SPDX-License-Identifier: Apache-2.0
//
// The real port_display for this board: EPDiy driving the ED047TC1, chosen in
// docs/adr/0007-epaper-backend.md (which records the reversal of its own
// earlier FastEPD decision and why).
//
// EPDiy arrives as an ordinary ESP-IDF managed component, pinned in
// apps/firmware_esp32/components/hac_platform/idf_component.yml. There is no
// vendor-boundary translation unit here, unlike the FastEPD integration this
// replaced: EPDiy has a normal C API and does not inline its implementation
// into whoever includes its header, so this file includes epdiy.h directly and
// still compiles under the project's full -Wall -Wextra -Werror -Wconversion
// -Wshadow.
//
// The core draws straight into EPDiy's own framebuffer -- this adapter
// allocates none. Two properties of EPDiy's 4 bpp buffer make that possible,
// and both were checked against its source rather than assumed
// (docs/adr/0003-gray4-nibble-order.md):
//
//   * the nibble order is this project's own -- odd x in the high nibble, even
//     x in the low (epdiy.c's epd_draw_pixel());
//   * the stride is width / 2, the same 480 bytes at 960 px that
//     framebuffer_required_size() produces.
//
// So get_framebuffer() hands back EPDiy's highlevel front buffer directly:
// no second allocation, no copy per flush, no nibble swap. Updates go through
// epd_hl_update_area() rather than epd_draw_base(), because only the highlevel
// layer keeps the previous-frame state the panel's waveform selection needs --
// adp_display.c's comment at s_hl explains what fails without it.
//
// The panel geometry and refresh limits reported here are the same BOARD_EPD_*
// constants core/app/update_policy.c has always read through the port; phase
// 2c set the partial-refresh budget from observation
// (docs/adr/0008-panel-timing.md).

#ifndef HAC_PLATFORM_ESP32_T5S3_ADP_DISPLAY_H
#define HAC_PLATFORM_ESP32_T5S3_ADP_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_status.h"
#include "port_display.h"
#include "port_log.h"
#include "ui/framebuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    port_display_t port;
    framebuffer_t fb;
    uint8_t *pixels;        // EPDiy's highlevel front buffer; NOT owned here
    const port_log_t *log;  // borrowed; must outlive this adapter

    // What every update hands EPDiy for its waveform's temperature range:
    // the measured temperature, or BOARD_EPD_ASSUMED_TEMPERATURE_C without
    // one (adp_display_set_temperature()).
    int temperature_c;
    bool temperature_measured;
} adp_display_t;

// Initializes EPDiy against this board's definition
// (epd_board_lilygo_t5_47_s3) and panel (ED047TC1), then wraps its highlevel
// front buffer as the framebuffer the core draws into. Logs which heap that
// buffer came from and how much PSRAM EPDiy's own buffers then left -- see
// docs/hardware/board_notes.md's memory budget for why both are observed
// rather than assumed. `log` may be NULL.
//
// Returns HAL_ERR_UNSUPPORTED if EPDiy reports a panel geometry other than
// the `width` x `height` asked for here, or a framebuffer too small for it --
// either would mean board_config.h and EPDiy's board definition disagree
// about the hardware, which is a decision rather than something to paper over
// at runtime. EPDiy asserts rather than returning on its own allocation
// failures, so there is no HAL_ERR_NO_MEM path left to report.
hal_status_t adp_display_init(adp_display_t *d, uint16_t width, uint16_t height,
                              const port_log_t *log);

port_display_t *adp_display_port(adp_display_t *d);

// The room temperature for the panel's waveform selection, as the
// composition measured it -- not through port_display, which knows nothing
// of temperature and does not need to: this is this panel's business, and
// the composition is what holds both the reading and the adapter. `known`
// false (no fresh reading) falls back to BOARD_EPD_ASSUMED_TEMPERATURE_C.
// Rounded to whole degrees, as EPDiy takes them; logged when it switches
// between measured and assumed.
//
// The ED047TC1 waveform EPDiy ships declares a single temperature range, so
// today any value draws the same way (board_config.h); this matters from the
// first waveform with several.
void adp_display_set_temperature(adp_display_t *d, bool known, float temperature_c);

void adp_display_deinit(adp_display_t *d);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_ADP_DISPLAY_H
