// SPDX-License-Identifier: Apache-2.0

#include "adp_display_sdl.h"

#define SDL_MAIN_HANDLED  // apps/simulator/main.c owns main(); SDL must not rename it
#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "epd_emulation.h"
#include "epd_model.h"
#include "ui/framebuffer.h"

// The outline drawn around the rectangle a flush touched. A window colour,
// not a panel property: chosen to be impossible to mistake for panel content,
// which is the whole job -- an update that claims to be partial but outlines
// the entire screen is then obvious at a glance.
#define HIGHLIGHT_R 220
#define HIGHLIGHT_G 0
#define HIGHLIGHT_B 160
#define HIGHLIGHT_THICKNESS 2

struct adp_display_sdl_s {
    port_display_t port;

    framebuffer_t fb;
    uint8_t *pixels;

    epd_model_t model;

    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;

    uint16_t width;
    uint16_t height;
    float zoom;
    double time_scale;

    // What the glass is currently showing, one GRAY4 level per pixel. It is
    // NOT the framebuffer: the core may have drawn a new frame that no flush
    // has pushed out yet, and the window must keep showing the old one.
    uint8_t *glass;

    // Residual charge left by partial refreshes, in 8-bit grey units. Darkens
    // what is displayed without touching the framebuffer.
    uint8_t *ghost;

    uint32_t *argb;  // scratch for the texture upload
};

static adp_display_sdl_t *self_of(port_display_t *port)
{
    return (adp_display_sdl_t *)port->impl;
}

// --- rendering -------------------------------------------------------------

static bool rect_contains(const rect_t *r, uint16_t x, uint16_t y)
{
    return x >= r->x && y >= r->y && (uint32_t)x < (uint32_t)r->x + r->w &&
           (uint32_t)y < (uint32_t)r->y + r->h;
}

// Paints the glass, minus accumulated ghosting, with `invert` (if given)
// showing the inversion flash of a full refresh over that region only.
static void render(adp_display_sdl_t *d, const rect_t *invert, const rect_t *highlight)
{
    for (uint16_t y = 0; y < d->height; ++y) {
        for (uint16_t x = 0; x < d->width; ++x) {
            const size_t i = (size_t)y * (size_t)d->width + (size_t)x;

            int32_t level = (int32_t)d->glass[i] * 17;  // 0-15 -> 0-255
            level -= (int32_t)d->ghost[i];
            if (level < 0) {
                level = 0;
            }
            if (invert != NULL && rect_contains(invert, x, y)) {
                level = 255 - level;
            }

            const uint32_t g = (uint32_t)level;
            d->argb[i] = 0xFF000000u | (g << 16) | (g << 8) | g;
        }
    }

    SDL_UpdateTexture(d->texture, NULL, d->argb, (int)((size_t)d->width * sizeof(uint32_t)));
    SDL_RenderClear(d->renderer);
    SDL_RenderCopy(d->renderer, d->texture, NULL, NULL);

    if (highlight != NULL && highlight->w > 0u && highlight->h > 0u) {
        SDL_SetRenderDrawColor(d->renderer, HIGHLIGHT_R, HIGHLIGHT_G, HIGHLIGHT_B,
                               SDL_ALPHA_OPAQUE);
        for (int t = 0; t < HIGHLIGHT_THICKNESS; ++t) {
            SDL_Rect r;
            r.x = (int)((float)highlight->x * d->zoom) + t;
            r.y = (int)((float)highlight->y * d->zoom) + t;
            r.w = (int)((float)highlight->w * d->zoom) - 2 * t;
            r.h = (int)((float)highlight->h * d->zoom) - 2 * t;
            if (r.w > 0 && r.h > 0) {
                SDL_RenderDrawRect(d->renderer, &r);
            }
        }
    }

    SDL_RenderPresent(d->renderer);
}

void adp_display_sdl_present(adp_display_sdl_t *d)
{
    render(d, NULL, NULL);
}

// Waits out an emulated panel duration in real time, compressed by the
// simulator's time scale. The log always reports the true panel duration.
static void wait_panel(const adp_display_sdl_t *d, uint32_t simulated_ms)
{
    double real_ms = (double)simulated_ms;
    if (d->time_scale > 1.0) {
        real_ms /= d->time_scale;
    }
    if (real_ms < 1.0) {
        return;
    }
    SDL_Delay((Uint32)real_ms);
}

// --- the panel -------------------------------------------------------------

static framebuffer_t *sdl_get_framebuffer(port_display_t *port)
{
    return &self_of(port)->fb;
}

// Moves the flushed region from the framebuffer onto the glass. A full
// refresh clears the residual charge it carried; a partial one adds to it
// wherever a pixel actually changed, which is where real ghosting comes from.
static void transfer(adp_display_sdl_t *d, const rect_t *area, bool full)
{
    for (uint16_t y = area->y; (uint32_t)y < (uint32_t)area->y + area->h; ++y) {
        for (uint16_t x = area->x; (uint32_t)x < (uint32_t)area->x + area->w; ++x) {
            const size_t i = (size_t)y * (size_t)d->width + (size_t)x;
            const uint8_t next = framebuffer_get_pixel(&d->fb, (int32_t)x, (int32_t)y);

            if (full) {
                d->ghost[i] = 0u;
            } else if (next != d->glass[i]) {
                uint32_t g = (uint32_t)d->ghost[i] + EPD_SIM_GHOST_PER_PARTIAL;
                if (g > EPD_SIM_GHOST_MAX) {
                    g = EPD_SIM_GHOST_MAX;
                }
                d->ghost[i] = (uint8_t)g;
            }
            d->glass[i] = next;
        }
    }
}

static hal_status_t sdl_flush(port_display_t *port, const rect_t *area, refresh_mode_t mode)
{
    adp_display_sdl_t *d = self_of(port);

    epd_flush_t f;
    epd_model_flush(&d->model, area, mode, &f);
    epd_model_log(&d->model, &f);

    if (f.area.w == 0u || f.area.h == 0u) {
        return HAL_OK;
    }

    const bool full = (f.performed == REFRESH_FULL || f.performed == REFRESH_CLEAR);
    uint32_t remaining_ms = f.duration_ms;

    if (full) {
        // The inversion flash: the old image goes negative before the new one
        // settles. It is the most recognizable thing a full refresh does, and
        // seeing it too often on a PC is the cheapest possible warning that a
        // refresh strategy is wrong.
        render(d, &f.area, NULL);
        wait_panel(d, EPD_SIM_FLASH_MS);
        remaining_ms = (remaining_ms > EPD_SIM_FLASH_MS) ? remaining_ms - EPD_SIM_FLASH_MS : 0u;
    }

    transfer(d, &f.area, full);

    uint32_t highlight_ms = remaining_ms;
    if (highlight_ms < EPD_SIM_HIGHLIGHT_MIN_MS) {
        highlight_ms = EPD_SIM_HIGHLIGHT_MIN_MS;
    }
    render(d, NULL, &f.area);
    wait_panel(d, highlight_ms);

    render(d, NULL, NULL);
    framebuffer_reset_dirty(&d->fb);
    return HAL_OK;
}

static hal_status_t sdl_power(port_display_t *port, bool on)
{
    adp_display_sdl_t *d = self_of(port);

    printf("[epd] power %s\n", on ? "on" : "off");
    if (!on) {
        // A powered-down panel keeps its last image: e-paper is bistable.
        // Nothing to do but say so.
        render(d, NULL, NULL);
    }
    return HAL_OK;
}

// --- lifecycle -------------------------------------------------------------

adp_display_sdl_t *adp_display_sdl_create(uint16_t width, uint16_t height, float zoom,
                                          double time_scale)
{
    if (zoom <= 0.0f) {
        zoom = 1.0f;
    }
    if (time_scale <= 0.0) {
        time_scale = 1.0;
    }

    adp_display_sdl_t *d = calloc(1u, sizeof(*d));
    if (d == NULL) {
        return NULL;
    }

    d->width = width;
    d->height = height;
    d->zoom = zoom;
    d->time_scale = time_scale;

    const size_t px_count = (size_t)width * (size_t)height;
    const size_t fb_size = framebuffer_required_size(width, height, PIXFMT_GRAY4);

    d->pixels = calloc(1u, fb_size);
    d->glass = calloc(1u, px_count);
    d->ghost = calloc(1u, px_count);
    d->argb = calloc(px_count, sizeof(uint32_t));
    if (d->pixels == NULL || d->glass == NULL || d->ghost == NULL || d->argb == NULL) {
        adp_display_sdl_destroy(d);
        return NULL;
    }

    // A panel that has never been refreshed is white, like paper.
    memset(d->glass, 0x0F, px_count);

    framebuffer_init(&d->fb, d->pixels, fb_size, width, height, PIXFMT_GRAY4);
    framebuffer_clear(&d->fb, 0x0Fu);
    epd_model_init(&d->model, width, height, EPD_SIM_MAX_PARTIAL_REFRESHES_BEFORE_FULL);

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        adp_display_sdl_destroy(d);
        return NULL;
    }

    const int win_w = (int)((float)width * zoom);
    const int win_h = (int)((float)height * zoom);

    d->window = SDL_CreateWindow("Home Air Monitor - ED047TC1 960x540", SDL_WINDOWPOS_CENTERED,
                                 SDL_WINDOWPOS_CENTERED, win_w, win_h, SDL_WINDOW_SHOWN);
    if (d->window == NULL) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        adp_display_sdl_destroy(d);
        return NULL;
    }

    d->renderer = SDL_CreateRenderer(d->window, -1, SDL_RENDERER_ACCELERATED);
    if (d->renderer == NULL) {
        d->renderer = SDL_CreateRenderer(d->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (d->renderer == NULL) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        adp_display_sdl_destroy(d);
        return NULL;
    }

    d->texture = SDL_CreateTexture(d->renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING, (int)width, (int)height);
    if (d->texture == NULL) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        adp_display_sdl_destroy(d);
        return NULL;
    }

    d->port.width = width;
    d->port.height = height;
    d->port.format = PIXFMT_GRAY4;
    d->port.min_full_refresh_interval_ms = EPD_SIM_MIN_FULL_REFRESH_INTERVAL_MS;
    d->port.max_partial_refreshes_before_full = EPD_SIM_MAX_PARTIAL_REFRESHES_BEFORE_FULL;
    d->port.get_framebuffer = sdl_get_framebuffer;
    d->port.flush = sdl_flush;
    d->port.power = sdl_power;
    d->port.impl = d;

    render(d, NULL, NULL);
    return d;
}

void adp_display_sdl_destroy(adp_display_sdl_t *d)
{
    if (d == NULL) {
        return;
    }
    if (d->texture != NULL) {
        SDL_DestroyTexture(d->texture);
    }
    if (d->renderer != NULL) {
        SDL_DestroyRenderer(d->renderer);
    }
    if (d->window != NULL) {
        SDL_DestroyWindow(d->window);
    }
    if (SDL_WasInit(SDL_INIT_VIDEO) != 0u) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    free(d->pixels);
    free(d->glass);
    free(d->ghost);
    free(d->argb);
    free(d);
}

port_display_t *adp_display_sdl_port(adp_display_sdl_t *d)
{
    return &d->port;
}

const epd_model_t *adp_display_sdl_model(const adp_display_sdl_t *d)
{
    return &d->model;
}
