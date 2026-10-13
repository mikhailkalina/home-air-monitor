// SPDX-License-Identifier: Apache-2.0
//
// The slideshow test app on a PC: composition and a wait loop around
// apps/slideshow/slideshow.c, the same shape as apps/simulator/main.c. Every
// picture embedded at build time is shown in turn, one REFRESH_FULL each,
// through the simulator's e-paper emulation -- the latency of each refresh,
// the flashing of its clear (platform/host/epd_emulation.h).
//
//   slideshow                            window; a picture a minute, in real time
//   slideshow --time-scale 20            window; each minute passes in 3 s
//   slideshow --headless --frames out/   one PNG per picture, no waiting
//
// The pictures are apps/slideshow/images/ unless the build was configured
// with -DSLIDESHOW_IMAGES_DIR=<dir> (apps/slideshow/slideshow_images.cmake).

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_display_png.h"
#include "epd_model.h"
#include "host_time.h"
#include "port_clock.h"
#include "port_display.h"
#include "slideshow.h"
#include "slideshow_images.generated.h"

#ifdef HAC_SIM_GUI
#include "adp_display_sdl.h"
#include "adp_input_sdl.h"
#include "port_input.h"
#endif

// The panel this build targets (docs/hardware/board_notes.md), as in
// apps/simulator/main.c. It reaches the slideshow only through port_display.
#define SLIDESHOW_PANEL_WIDTH 960u
#define SLIDESHOW_PANEL_HEIGHT 540u

// What was asked for: "a picture a minute". The panel's minimum full-refresh
// interval is applied on top by slideshow_init().
#define SLIDESHOW_DEFAULT_INTERVAL_S 60u

// White around a picture smaller than the panel, as the product's screens use.
#define SLIDESHOW_BACKGROUND 15u

// Windowed: how long one loop iteration waits before looking again.
#define SLIDESHOW_TICK_MS 20u

typedef struct {
    const char *frames_dir;
    double time_scale;
    float zoom;
    bool headless;
    bool caption;
    uint32_t interval_s;
    uint32_t duration_s;  // 0 = until the window is closed
    bool duration_set;
} args_t;

static void usage(const char *argv0)
{
    printf("usage: %s [options]\n"
           "\n"
           "  --interval-s <n>   seconds from one picture's refresh to the next (default %u;\n"
           "                     never less than the panel's minimum full-refresh interval)\n"
           "  --time-scale <n>   virtual seconds per real second (default 1)\n"
           "  --headless         no window: write frames instead\n"
           "  --frames <dir>     directory for the headless PNG frames\n"
           "  --duration <s>     stop after this many virtual seconds (headless default:\n"
           "                     one picture each)\n"
           "  --zoom <f>         window scale, 1 = one panel pixel per screen pixel\n"
           "  --no-caption       leave out the \"n/N name\" box\n"
           "  --help\n"
           "\n"
           "%u picture(s) embedded at build time.\n",
           argv0, SLIDESHOW_DEFAULT_INTERVAL_S, SLIDESHOW_IMAGES_COUNT);
}

static bool parse_args(int argc, char **argv, args_t *out)
{
    memset(out, 0, sizeof(*out));
    out->time_scale = 1.0;
    out->zoom = 1.0f;
    out->caption = true;
    out->interval_s = SLIDESHOW_DEFAULT_INTERVAL_S;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const bool has_value = (i + 1 < argc);

#define TAKE_VALUE(name)                             \
    if (!has_value) {                                \
        fprintf(stderr, "%s needs a value\n", name); \
        return false;                                \
    }

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            exit(0);
        } else if (strcmp(arg, "--interval-s") == 0) {
            TAKE_VALUE("--interval-s");
            out->interval_s = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(arg, "--time-scale") == 0) {
            TAKE_VALUE("--time-scale");
            out->time_scale = atof(argv[++i]);
            if (out->time_scale <= 0.0) {
                fprintf(stderr, "--time-scale must be greater than 0\n");
                return false;
            }
        } else if (strcmp(arg, "--frames") == 0) {
            TAKE_VALUE("--frames");
            out->frames_dir = argv[++i];
        } else if (strcmp(arg, "--duration") == 0) {
            TAKE_VALUE("--duration");
            out->duration_s = (uint32_t)strtoul(argv[++i], NULL, 10);
            out->duration_set = true;
        } else if (strcmp(arg, "--zoom") == 0) {
            TAKE_VALUE("--zoom");
            out->zoom = (float)atof(argv[++i]);
            if (out->zoom <= 0.0f) {
                fprintf(stderr, "--zoom must be greater than 0\n");
                return false;
            }
        } else if (strcmp(arg, "--headless") == 0) {
            out->headless = true;
        } else if (strcmp(arg, "--no-caption") == 0) {
            out->caption = false;
        } else {
            fprintf(stderr, "unknown option: %s\n", arg);
            usage(argv[0]);
            return false;
        }
#undef TAKE_VALUE
    }

    if (out->frames_dir != NULL) {
        out->headless = true;  // as in the simulator: --frames means "write them"
    }
    return true;
}

// Windowed: virtual time follows real time through the scale. Also called
// straight after a flush, so that the slideshow is told the time the
// refresh ended -- the SDL adapter really waits it out -- as the panel's own
// loop is.
static void follow_real_time(adp_clock_virtual_t *vclock, uint64_t *last_real_ms)
{
    const uint64_t now_real = host_monotonic_ms();
    const uint64_t elapsed = (now_real >= *last_real_ms) ? now_real - *last_real_ms : 0u;
    *last_real_ms = now_real;
    adp_clock_virtual_advance(vclock, adp_clock_virtual_scale_ms(vclock, elapsed));
}

static bool show_next(slideshow_t *show, port_display_t *display, const port_clock_t *clock)
{
    framebuffer_t *fb = display->get_framebuffer(display);
    if (fb == NULL) {
        return false;
    }

    const size_t index = slideshow_current(show);
    const uint64_t start_ms = clock->now_ms(clock);
    slideshow_render(show, fb);
    const hal_status_t status = display->flush(display, NULL, REFRESH_FULL);
    printf("[slideshow] t=%llu ms  picture %zu/%zu \"%s\"  REFRESH_FULL %s\n",
           (unsigned long long)start_ms, index + 1u, (size_t)SLIDESHOW_IMAGES_COUNT,
           slideshow_current_name(show), (status == HAL_OK) ? "ok" : "FAILED");
    return status == HAL_OK;
}

int main(int argc, char **argv)
{
    args_t args;
    if (!parse_args(argc, argv, &args)) {
        return 2;
    }

    adp_clock_virtual_t vclock;
    adp_clock_virtual_init(&vclock, args.time_scale);
    const port_clock_t *clock = adp_clock_virtual_port(&vclock);

    adp_display_png_t png_display;
    port_display_t *display = NULL;
    const epd_model_t *panel_model = NULL;

#ifdef HAC_SIM_GUI
    adp_display_sdl_t *sdl_display = NULL;
    adp_input_sdl_t sdl_input;
#endif

    if (args.headless) {
        const hal_status_t st =
            adp_display_png_init(&png_display, (uint16_t)SLIDESHOW_PANEL_WIDTH,
                                 (uint16_t)SLIDESHOW_PANEL_HEIGHT, args.frames_dir);
        if (st != HAL_OK) {
            fprintf(stderr, "cannot start the headless display (status %d)\n", (int)st);
            return 1;
        }
        display = adp_display_png_port(&png_display);
        panel_model = &png_display.model;
    } else {
#ifdef HAC_SIM_GUI
        sdl_display =
            adp_display_sdl_create((uint16_t)SLIDESHOW_PANEL_WIDTH,
                                   (uint16_t)SLIDESHOW_PANEL_HEIGHT, args.zoom, args.time_scale);
        if (sdl_display == NULL) {
            return 1;
        }
        display = adp_display_sdl_port(sdl_display);
        panel_model = adp_display_sdl_model(sdl_display);
        adp_input_sdl_init(&sdl_input, (uint16_t)SLIDESHOW_PANEL_WIDTH,
                           (uint16_t)SLIDESHOW_PANEL_HEIGHT, args.zoom, clock);
#else
        fprintf(stderr, "this build has no window (SIM_GUI=OFF, or no SDL2). Re-run with "
                        "--headless --frames <dir>.\n");
        return 1;
#endif
    }

    const slideshow_config_t cfg = {
        .interval_ms = args.interval_s * 1000u,
        .background = SLIDESHOW_BACKGROUND,
        .caption = args.caption,
    };
    slideshow_t show;
    slideshow_init(&show, slideshow_images_all, slideshow_images_names, SLIDESHOW_IMAGES_COUNT,
                   &cfg, display->min_full_refresh_interval_ms);

    if (cfg.interval_ms != slideshow_interval_ms(&show)) {
        printf("[slideshow] interval raised from %u s to the panel's minimum full-refresh "
               "interval, %u ms\n",
               args.interval_s, display->min_full_refresh_interval_ms);
    }
    printf("[slideshow] %zu picture(s), %zu bytes of pixels, one every %u ms after the "
           "previous refresh; %s, time scale x%.4g\n",
           (size_t)SLIDESHOW_IMAGES_COUNT, slideshow_pixel_bytes(&show),
           slideshow_interval_ms(&show), args.headless ? "headless" : "windowed", args.time_scale);

    // Headless with no --duration: one picture each, then stop -- the frames
    // are the point, and a run that never ends would hang CI.
    uint64_t stop_at_ms = (uint64_t)args.duration_s * 1000u;
    if (args.headless && !args.duration_set) {
        stop_at_ms = (uint64_t)slideshow_interval_ms(&show) * (SLIDESHOW_IMAGES_COUNT - 1u) + 1u;
    }

    uint64_t last_real_ms = host_monotonic_ms();
    bool running = true;

    while (running) {
        if (args.headless) {
            // Nothing to watch: jump straight to the next picture.
            const uint64_t now_ms = clock->now_ms(clock);
            const uint64_t due_ms = slideshow_next_due_ms(&show);
            if (due_ms > now_ms) {
                adp_clock_virtual_advance(&vclock, due_ms - now_ms);
            }
        } else {
            follow_real_time(&vclock, &last_real_ms);
        }

        const uint64_t now_ms = clock->now_ms(clock);
        if (stop_at_ms != 0u && now_ms >= stop_at_ms) {
            break;
        }

        if (now_ms >= slideshow_next_due_ms(&show)) {
            const bool ok = show_next(&show, display, clock);
            if (!args.headless) {
                follow_real_time(&vclock, &last_real_ms);
            }
            slideshow_flushed(&show, clock->now_ms(clock), ok);
        }

#ifdef HAC_SIM_GUI
        if (sdl_display != NULL) {
            input_event_t ev;
            port_input_t *input = adp_input_sdl_port(&sdl_input);
            while (input->poll(input, &ev)) {
                // Touch does nothing here; polling is what pumps SDL's events.
            }
            adp_display_sdl_present(sdl_display);
            if (adp_input_sdl_quit_requested(&sdl_input)) {
                running = false;
            }
            host_sleep_ms(SLIDESHOW_TICK_MS);
        }
#endif
    }

    if (panel_model != NULL) {
        epd_model_log_summary(panel_model);
    }
#ifdef HAC_SIM_GUI
    if (sdl_display != NULL) {
        adp_display_sdl_destroy(sdl_display);
    }
#endif
    if (args.headless) {
        adp_display_png_deinit(&png_display);
    }
    return 0;
}
