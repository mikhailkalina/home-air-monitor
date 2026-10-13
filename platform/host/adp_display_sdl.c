// SPDX-License-Identifier: Apache-2.0

#include "adp_display_sdl.h"

#define SDL_MAIN_HANDLED  // apps/simulator/main.c owns main(); SDL must not rename it
#include <SDL.h>

#include <stdint.h>
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

    // Per pixel, the partial refreshes since it was last driven (counting only
    // those whose rectangle covered it), saturating. A dark pixel left
    // undriven long enough shows lighter than freshly driven ones -- the
    // outline ghosting phase 2c observed; see epd_emulation.h. Never touches
    // the framebuffer.
    uint8_t *undriven;

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

// What one glass pixel looks like, in 8-bit grey: its GRAY4 level as this
// unit shows it (levels it cannot tell apart are merged), lightened if it is a
// dark pixel that partial refreshes have left undriven for long enough.
static int32_t displayed_level(const adp_display_sdl_t *d, size_t i)
{
    uint8_t gray4 = d->glass[i];
    if (gray4 <= EPD_SIM_DARKEST_MERGED_LEVEL) {
        gray4 = 0u;
    }

    int32_t level = (int32_t)gray4 * 17;  // 0-15 -> 0-255
    if (d->glass[i] < EPD_SIM_GHOST_DARK_BELOW && d->undriven[i] >= EPD_SIM_GHOST_VISIBLE_AFTER) {
        level += (int32_t)EPD_SIM_GHOST_LIGHTEN;
    }
    return (level > 255) ? 255 : level;
}

// Paints the glass, with `solid` (if given) overridden by one flat 8-bit grey
// -- what the clear's black and white pushes show over the refreshed region.
static void render(adp_display_sdl_t *d, const rect_t *solid, uint8_t solid_level,
                   const rect_t *highlight)
{
    for (uint16_t y = 0; y < d->height; ++y) {
        for (uint16_t x = 0; x < d->width; ++x) {
            const size_t i = (size_t)y * (size_t)d->width + (size_t)x;

            int32_t level = displayed_level(d, i);
            if (solid != NULL && rect_contains(solid, x, y)) {
                level = (int32_t)solid_level;
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
    render(d, NULL, 0u, NULL);
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

// The clear as the glass shows it: EPD_SIM_CLEAR_CYCLES rounds of the area
// going black, then white, sharing EPD_SIM_CLEAR_REFRESH_MS out over the
// pushes in proportion. Waited out like any other panel time, so it scales
// with --time-scale.
static void flash_clear(adp_display_sdl_t *d, const rect_t *area)
{
    const uint32_t pushes_per_cycle = EPD_SIM_CLEAR_DARK_PUSHES + EPD_SIM_CLEAR_WHITE_PUSHES;
    const uint32_t pushes = EPD_SIM_CLEAR_CYCLES * pushes_per_cycle;
    const uint32_t dark_ms = EPD_SIM_CLEAR_REFRESH_MS * EPD_SIM_CLEAR_DARK_PUSHES / pushes;
    const uint32_t white_ms = EPD_SIM_CLEAR_REFRESH_MS * EPD_SIM_CLEAR_WHITE_PUSHES / pushes;

    for (uint32_t c = 0u; c < EPD_SIM_CLEAR_CYCLES; ++c) {
        render(d, area, 0u, NULL);
        wait_panel(d, dark_ms);
        render(d, area, 255u, NULL);
        wait_panel(d, white_ms);
    }
}

// --- the panel -------------------------------------------------------------

static framebuffer_t *sdl_get_framebuffer(port_display_t *port)
{
    return &self_of(port)->fb;
}

// Moves the flushed region onto the glass, the way the panel does it:
//
//   REFRESH_FULL  -- every pixel redriven to the framebuffer's value;
//   REFRESH_CLEAR -- every pixel driven to white, the framebuffer ignored
//                    (platform/esp32_t5s3/adp_display.c leaves it alone);
//   partial       -- only pixels whose value changed are driven; the rest
//                    age by one, which is what the outline ghosting grows from.
static void transfer(adp_display_sdl_t *d, const rect_t *area, refresh_mode_t performed)
{
    for (uint16_t y = area->y; (uint32_t)y < (uint32_t)area->y + area->h; ++y) {
        for (uint16_t x = area->x; (uint32_t)x < (uint32_t)area->x + area->w; ++x) {
            const size_t i = (size_t)y * (size_t)d->width + (size_t)x;

            if (performed == REFRESH_CLEAR) {
                d->glass[i] = EPD_MODEL_GRAY4_WHITE;
                d->undriven[i] = 0u;
                continue;
            }

            const uint8_t next = framebuffer_get_pixel(&d->fb, (int32_t)x, (int32_t)y);
            if (performed == REFRESH_FULL || next != d->glass[i]) {
                d->glass[i] = next;
                d->undriven[i] = 0u;
            } else if (d->undriven[i] < UINT8_MAX) {
                d->undriven[i]++;
            }
        }
    }
}

static hal_status_t sdl_flush(port_display_t *port, const rect_t *area, refresh_mode_t mode)
{
    adp_display_sdl_t *d = self_of(port);

    epd_flush_t f;
    epd_model_flush(&d->model, area, mode, &d->fb, d->glass, &f);
    epd_model_log(&d->model, &f);

    if (f.area.w == 0u || f.area.h == 0u) {
        return HAL_OK;
    }

    const bool full = (f.performed == REFRESH_FULL || f.performed == REFRESH_CLEAR);
    uint32_t remaining_ms = f.duration_ms;

    if (full) {
        // The clear's black and white flashing. It is the most recognizable
        // thing a full refresh does, and seeing it too often on a PC is the
        // cheapest possible warning that a refresh strategy is wrong.
        flash_clear(d, &f.area);
        remaining_ms = (remaining_ms > EPD_SIM_CLEAR_REFRESH_MS)
                           ? remaining_ms - EPD_SIM_CLEAR_REFRESH_MS
                           : 0u;
    }

    transfer(d, &f.area, f.performed);

    uint32_t highlight_ms = remaining_ms;
    if (highlight_ms < EPD_SIM_HIGHLIGHT_MIN_MS) {
        highlight_ms = EPD_SIM_HIGHLIGHT_MIN_MS;
    }
    render(d, NULL, 0u, &f.area);
    wait_panel(d, highlight_ms);

    render(d, NULL, 0u, NULL);
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
        render(d, NULL, 0u, NULL);
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
    d->undriven = calloc(1u, px_count);
    d->argb = calloc(px_count, sizeof(uint32_t));
    if (d->pixels == NULL || d->glass == NULL || d->undriven == NULL || d->argb == NULL) {
        adp_display_sdl_destroy(d);
        return NULL;
    }

    // A panel that has never been refreshed is white, like paper.
    memset(d->glass, EPD_MODEL_GRAY4_WHITE, px_count);

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

    render(d, NULL, 0u, NULL);
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
    free(d->undriven);
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
