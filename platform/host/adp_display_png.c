// SPDX-License-Identifier: Apache-2.0

#include "adp_display_png.h"

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

// The panel shows 16 grey levels; the PNG is 8-bit grey, so a level maps to
// level * 17 and 15 lands exactly on 255.
static void to_gray8(const framebuffer_t *fb, uint8_t *out)
{
    for (uint16_t y = 0; y < fb->height; ++y) {
        for (uint16_t x = 0; x < fb->width; ++x) {
            const uint8_t v = framebuffer_get_pixel(fb, x, y);
            const uint8_t gray8 =
                (fb->format == PIXFMT_GRAY4) ? (uint8_t)(v * 17) : (uint8_t)(v != 0 ? 255 : 0);
            out[(size_t)y * (size_t)fb->width + (size_t)x] = gray8;
        }
    }
}

static hal_status_t png_flush(port_display_t *port, const rect_t *area, refresh_mode_t mode)
{
    adp_display_png_t *d = self_of(port);

    epd_flush_t flush;
    epd_model_flush(&d->model, area, mode, &flush);
    epd_model_log(&d->model, &flush);

    framebuffer_reset_dirty(&d->fb);

    if (!d->write_frames) {
        return HAL_OK;
    }

    // The whole panel is written every time, not just the flushed rectangle:
    // a frame dump is meant to be the picture a person would be looking at.
    to_gray8(&d->fb, d->scratch);

    char path[600];
    snprintf(path, sizeof(path), "%s/frame_%04u.png", d->out_dir, d->frame_index);

    if (!png_write_gray8(path, d->scratch, d->fb.width, d->fb.height)) {
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
    const size_t scratch_size = (size_t)width * (size_t)height;

    d->pixels = calloc(1u, fb_size);
    d->scratch = calloc(1u, scratch_size);
    if (d->pixels == NULL || d->scratch == NULL) {
        adp_display_png_deinit(d);
        return HAL_ERR_NO_MEM;
    }

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
    free(d->scratch);
    d->pixels = NULL;
    d->scratch = NULL;
}
