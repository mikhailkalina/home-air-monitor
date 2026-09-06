// SPDX-License-Identifier: Apache-2.0
//
// The display port.
//
// The core draws into a framebuffer the platform owns and then asks for it to
// be pushed to the panel. Everything panel-specific -- waveforms, the SPI/I2S
// bus, PSRAM placement, the EPDiy or SDL backend behind it -- stays in the
// adapter (docs/architecture.md 5.5).
//
// Two fields flow the other way, from the adapter up into the core:
// min_full_refresh_interval_ms and max_partial_refreshes_before_full. They are
// properties of the panel, and the adapter is the only layer that knows them.
// The policy that applies them lives in core/app/update_policy.c, which reads
// them through update_policy_limits_from_display() and holds no constant of
// its own for any particular panel.

#ifndef HAC_PORTS_PORT_DISPLAY_H
#define HAC_PORTS_PORT_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_status.h"
#include "ui/framebuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    REFRESH_FULL,       // full refresh with inversion; clears accumulated ghosting
    REFRESH_PARTIAL,    // fast, but ghosting accumulates and the panel degrades
    REFRESH_FAST_MONO,  // fast black-and-white mode, where the panel offers one
    REFRESH_CLEAR,      // clear / panel recovery
} refresh_mode_t;

typedef struct port_display_s port_display_t;

struct port_display_s {
    uint16_t width;
    uint16_t height;
    pixel_format_t format;

    // Shortest time the panel tolerates between two REFRESH_FULL flushes.
    // 0 means the panel imposes no minimum.
    uint32_t min_full_refresh_interval_ms;

    // How many REFRESH_PARTIAL flushes may follow a REFRESH_FULL before the
    // panel needs a full one again. The vendor of the ED047TC1 warns that
    // prolonged partial refreshing leaves residual images and causes
    // irreversible damage (docs/hardware/board_notes.md), so this is a
    // hardware limit, not a preference.
    //
    // 0 means the panel offers no usable partial mode: every flush is full.
    uint32_t max_partial_refreshes_before_full;

    // The buffer the core draws into. Owned by the adapter (PSRAM on the
    // ESP32-S3, malloc on a PC); the core never allocates one. Returns NULL
    // if the display failed to initialize.
    framebuffer_t *(*get_framebuffer)(port_display_t *self);

    // Pushes `area` (NULL means the whole panel) to the glass in `mode`. The
    // call blocks for as long as the refresh takes -- hundreds of
    // milliseconds on real e-paper, and the simulator emulates that rather
    // than returning instantly, so a chatty refresh strategy is felt on a PC.
    hal_status_t (*flush)(port_display_t *self, const rect_t *area, refresh_mode_t mode);

    hal_status_t (*power)(port_display_t *self, bool on);

    void *impl;
};

#ifdef __cplusplus
}
#endif

#endif  // HAC_PORTS_PORT_DISPLAY_H
