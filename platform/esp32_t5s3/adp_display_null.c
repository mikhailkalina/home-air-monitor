// SPDX-License-Identifier: Apache-2.0

#include "adp_display_null.h"

#include <string.h>

#include "esp_heap_caps.h"

#include "board_config.h"

static adp_display_null_t *self_of(port_display_t *port)
{
    return (adp_display_null_t *)port->impl;
}

static framebuffer_t *null_get_framebuffer(port_display_t *port)
{
    adp_display_null_t *d = self_of(port);
    return (d->pixels != NULL) ? &d->fb : NULL;
}

static const char *refresh_mode_name(refresh_mode_t mode)
{
    switch (mode) {
        case REFRESH_FULL:
            return "full";
        case REFRESH_PARTIAL:
            return "partial";
        case REFRESH_FAST_MONO:
            return "fast_mono";
        case REFRESH_CLEAR:
            return "clear";
        default:
            return "?";
    }
}

static hal_status_t null_flush(port_display_t *port, const rect_t *area, refresh_mode_t mode)
{
    adp_display_null_t *d = self_of(port);

    const rect_t whole_panel = {0u, 0u, d->port.width, d->port.height};
    const rect_t resolved = (area != NULL) ? *area : whole_panel;

    // `resolved` is the rect that will actually reach the panel: for a
    // partial refresh, this is apps/firmware_esp32/main/event_loop.c's
    // framebuffer_diff_dirty_rect() result against the previous frame, not
    // everything screen_home_render() touched -- that would always be the
    // whole panel, since it redraws unconditionally every time (see the
    // fb.dirty_rect comment above framebuffer_reset_dirty() below). A
    // sub-region here is the signal that the diff, not just the render,
    // survived the trip to real hardware; it must match what
    // apps/simulator/main.c's SDL highlight shows, since both read this same
    // core-computed rect.
    const log_field_t dirty_fields[] = {
        log_u64("dirty_x", resolved.x),           log_u64("dirty_y", resolved.y),
        log_u64("dirty_w", resolved.w),           log_u64("dirty_h", resolved.h),
        log_str("mode", refresh_mode_name(mode)),
    };
    port_log_write(d->log, LOG_LEVEL_INFO, "adp_display_null", "flush", dirty_fields,
                   sizeof(dirty_fields) / sizeof(dirty_fields[0]));

    // fb.dirty_rect itself (unrelated to `resolved` above) still reports
    // whatever screen_home_render() touched this frame -- the whole panel,
    // always, since it redraws unconditionally. Reset for the next render;
    // not logged, since it no longer answers a question phase 2c needs.
    framebuffer_reset_dirty(&d->fb);
    return HAL_OK;
}

static hal_status_t null_power(port_display_t *port, bool on)
{
    adp_display_null_t *d = self_of(port);

    const log_field_t fields[] = {log_bool("on", on)};
    port_log_write(d->log, LOG_LEVEL_INFO, "adp_display_null", "power", fields, 1u);
    return HAL_OK;
}

hal_status_t adp_display_null_init(adp_display_null_t *d, uint16_t width, uint16_t height,
                                   const port_log_t *log)
{
    memset(d, 0, sizeof(*d));
    d->log = log;

    const size_t fb_size = framebuffer_required_size(width, height, PIXFMT_GRAY4);

    d->pixels = heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (d->pixels == NULL) {
        return HAL_ERR_NO_MEM;
    }

    framebuffer_init(&d->fb, d->pixels, fb_size, width, height, PIXFMT_GRAY4);

    // An unrefreshed panel is white, like paper -- matches
    // platform/host/adp_display_sdl.c so the device and the simulator start
    // from the same image before the first render.
    framebuffer_clear(&d->fb, 0x0Fu);

    d->port.width = width;
    d->port.height = height;
    d->port.format = PIXFMT_GRAY4;
    d->port.min_full_refresh_interval_ms = BOARD_EPD_MIN_FULL_REFRESH_INTERVAL_MS;
    d->port.max_partial_refreshes_before_full = BOARD_EPD_MAX_PARTIAL_REFRESHES_BEFORE_FULL;
    d->port.get_framebuffer = null_get_framebuffer;
    d->port.flush = null_flush;
    d->port.power = null_power;
    d->port.impl = d;

    return HAL_OK;
}

port_display_t *adp_display_null_port(adp_display_null_t *d)
{
    return &d->port;
}

void adp_display_null_deinit(adp_display_null_t *d)
{
    heap_caps_free(d->pixels);
    d->pixels = NULL;
}
