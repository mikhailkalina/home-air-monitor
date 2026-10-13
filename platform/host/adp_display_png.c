// SPDX-License-Identifier: Apache-2.0
//
// mkdir() is POSIX.1 (not C11), and the mode below strict-C11-builds this
// project with CMAKE_C_EXTENSIONS OFF. On a glibc target, that can hide
// <sys/stat.h>'s declaration under -std=c11 unless a feature-test macro asks
// for it explicitly -- the same class of build failure host_time.c hit for
// clock_gettime()/nanosleep(). The macro has to be defined before the first
// system header this translation unit includes -- so before "adp_display_png.h"
// itself -- or it is too late. 200809L is POSIX.1-2008, the ordinary baseline
// mkdir needs; it is not a realtime-specific level like host_time.c's 199309L,
// so claiming it here is not claiming more than this file actually uses.
#define _POSIX_C_SOURCE 200809L

#include "adp_display_png.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "epd_emulation.h"
#include "png_write.h"

#if defined(_WIN32)
#include <direct.h>
#define hac_mkdir(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define hac_mkdir(path) mkdir((path), 0755)
#endif

static adp_display_png_t *self_of(port_display_t *port)
{
    return (adp_display_png_t *)port->impl;
}

static framebuffer_t *png_get_framebuffer(port_display_t *port)
{
    return &self_of(port)->fb;
}

// Moves the flushed region onto the glass the way the panel does: a full
// refresh redraws everything, a clear leaves white, a partial copies the area.
static void transfer(adp_display_png_t *d, const epd_flush_t *f)
{
    for (uint32_t y = f->area.y; y < (uint32_t)f->area.y + f->area.h; ++y) {
        for (uint32_t x = f->area.x; x < (uint32_t)f->area.x + f->area.w; ++x) {
            const size_t i = (size_t)y * d->fb.width + x;
            d->glass[i] = (f->performed == REFRESH_CLEAR)
                              ? (uint8_t)EPD_MODEL_GRAY4_WHITE
                              : framebuffer_get_pixel(&d->fb, (int32_t)x, (int32_t)y);
        }
    }
}

static hal_status_t png_flush(port_display_t *port, const rect_t *area, refresh_mode_t mode)
{
    adp_display_png_t *d = self_of(port);

    epd_flush_t flush;
    epd_model_flush(&d->model, area, mode, &d->fb, d->glass, &flush);
    epd_model_log(&d->model, &flush);
    transfer(d, &flush);

    framebuffer_reset_dirty(&d->fb);

    if (!d->write_frames) {
        return HAL_OK;
    }

    // The whole panel is written every time, not just the flushed rectangle:
    // a frame dump is meant to be the picture a person would be looking at.
    char path[600];
    snprintf(path, sizeof(path), "%s/frame_%04u.png", d->out_dir, d->frame_index);

    if (!png_write_framebuffer_gray8(&d->fb, path)) {
        fprintf(stderr, "[epd] failed to write %s\n", path);
        return HAL_ERR_IO;
    }
    printf("[epd] wrote %s\n", path);
    d->frame_index++;
    return HAL_OK;
}

static hal_status_t png_power(port_display_t *port, bool on)
{
    (void)port;
    printf("[epd] power %s\n", on ? "on" : "off");
    return HAL_OK;
}

hal_status_t adp_display_png_init(adp_display_png_t *d, uint16_t width, uint16_t height,
                                  const char *out_dir)
{
    memset(d, 0, sizeof(*d));

    const size_t fb_size = framebuffer_required_size(width, height, PIXFMT_GRAY4);

    d->pixels = calloc(1u, fb_size);
    d->glass = malloc((size_t)width * (size_t)height);
    if (d->pixels == NULL || d->glass == NULL) {
        adp_display_png_deinit(d);
        return HAL_ERR_NO_MEM;
    }
    memset(d->glass, EPD_MODEL_GRAY4_WHITE, (size_t)width * (size_t)height);

    framebuffer_init(&d->fb, d->pixels, fb_size, width, height, PIXFMT_GRAY4);
    epd_model_init(&d->model, width, height, EPD_SIM_MAX_PARTIAL_REFRESHES_BEFORE_FULL);

    if (out_dir != NULL && out_dir[0] != '\0') {
        if (strlen(out_dir) >= sizeof(d->out_dir)) {
            adp_display_png_deinit(d);
            return HAL_ERR_INVALID_ARG;
        }
        strcpy(d->out_dir, out_dir);

        // An existing directory is the normal case, so a failure here is only
        // fatal if the directory is still not usable afterwards.
        (void)hac_mkdir(d->out_dir);

        char probe[600];
        snprintf(probe, sizeof(probe), "%s/.frames", d->out_dir);
        FILE *f = fopen(probe, "wb");
        if (f == NULL) {
            fprintf(stderr, "[epd] cannot write into %s\n", d->out_dir);
            adp_display_png_deinit(d);
            return HAL_ERR_IO;
        }
        fclose(f);
        remove(probe);

        d->write_frames = true;
    }

    d->port.width = width;
    d->port.height = height;
    d->port.format = PIXFMT_GRAY4;
    d->port.min_full_refresh_interval_ms = EPD_SIM_MIN_FULL_REFRESH_INTERVAL_MS;
    d->port.max_partial_refreshes_before_full = EPD_SIM_MAX_PARTIAL_REFRESHES_BEFORE_FULL;
    d->port.get_framebuffer = png_get_framebuffer;
    d->port.flush = png_flush;
    d->port.power = png_power;
    d->port.impl = d;

    return HAL_OK;
}

port_display_t *adp_display_png_port(adp_display_png_t *d)
{
    return &d->port;
}

void adp_display_png_deinit(adp_display_png_t *d)
{
    free(d->pixels);
    d->pixels = NULL;
    free(d->glass);
    d->glass = NULL;
}
