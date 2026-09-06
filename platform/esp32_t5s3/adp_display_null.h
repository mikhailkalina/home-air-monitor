// SPDX-License-Identifier: Apache-2.0
//
// A port_display that drives no hardware. It reports the real panel geometry
// and the real (still estimated -- board_config.h says so) refresh limits,
// and allocates the real 253 KB GRAY4 framebuffer in PSRAM, so that
// core/app/update_policy.c makes exactly the decisions it will make once
// phase 2b adds the panel driver. flush() does not touch the panel: it logs
// the resolved rectangle, the framebuffer's own dirty rectangle and the
// refresh mode through port_log, which is what phase 2a's bring-up run reads
// to confirm the dirty-rect tracking survived the trip to real hardware.

#ifndef HAC_PLATFORM_ESP32_T5S3_ADP_DISPLAY_NULL_H
#define HAC_PLATFORM_ESP32_T5S3_ADP_DISPLAY_NULL_H

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
    uint8_t *pixels;        // heap_caps_malloc'd from PSRAM; owned by this adapter
    const port_log_t *log;  // borrowed; must outlive this adapter
} adp_display_null_t;

// Allocates the framebuffer from PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT).
// `log` may be NULL, in which case flush() and power() are silent -- see
// port_log_write()'s NULL-safety.
//
// Returns HAL_ERR_NO_MEM if PSRAM cannot supply the buffer.
hal_status_t adp_display_null_init(adp_display_null_t *d, uint16_t width, uint16_t height,
                                   const port_log_t *log);

port_display_t *adp_display_null_port(adp_display_null_t *d);

void adp_display_null_deinit(adp_display_null_t *d);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_ADP_DISPLAY_NULL_H
