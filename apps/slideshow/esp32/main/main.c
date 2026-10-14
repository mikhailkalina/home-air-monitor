// SPDX-License-Identifier: Apache-2.0
//
// The slideshow test app on the board: composition and a wait loop around
// apps/slideshow/slideshow.c, with the panel driven by the product's own
// display adapter (platform/esp32_t5s3/adp_display.c, EPDiy). Every picture
// embedded at build time is shown in turn, one REFRESH_FULL each, roughly a
// minute apart (main/Kconfig.projbuild).
//
// The photo frame (apps/photo_frame/esp32/) builds this same file with its
// own pictures and the caption off.
//
// The loop runs on app_main's task, as the calibration image's does: nothing
// else shares the board, so there is no event loop to hand it to. That task's
// stack has to hold EPDiy's flush -- see the check below.
//
// Each refresh logs one line, grep-able in a pasted log:
//
//   slideshow: picture shown index=2 count=5 name=02_dither_methods status=0
//              flush_ms=3312 next_in_ms=60000

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include "adp_clock.h"
#include "adp_display.h"
#include "adp_log.h"
#include "board_config.h"

#include "hal_status.h"
#include "port_clock.h"
#include "port_display.h"
#include "port_log.h"

#include "slideshow.h"
#include "slideshow_images.generated.h"

#define SLIDESHOW_TAG "slideshow"

// EPDiy's flush overflowed the default 3584-byte main stack in the
// calibration image, which also flushes from app_main
// (apps/firmware_esp32/main/epd_calibration.c, CAL_MIN_MAIN_TASK_STACK_BYTES).
#define SLIDESHOW_MIN_MAIN_TASK_STACK_BYTES 8192

#if CONFIG_ESP_MAIN_TASK_STACK_SIZE < SLIDESHOW_MIN_MAIN_TASK_STACK_BYTES
#error "the slideshow flushes from app_main: needs ESP_MAIN_TASK_STACK_SIZE >= 8192"
#endif

#if CONFIG_HAC_SLIDESHOW_CAPTION
#define SLIDESHOW_CAPTION true
#else
#define SLIDESHOW_CAPTION false
#endif

#if CONFIG_HAC_SLIDESHOW_OVERRIDE_PANEL_MINIMUM
#define SLIDESHOW_OVERRIDE_PANEL_MINIMUM true
#else
#define SLIDESHOW_OVERRIDE_PANEL_MINIMUM false
#endif

// White around a picture smaller than the panel, as the product's screens use.
#define SLIDESHOW_BACKGROUND 15u

static void log_memory(const port_log_t *log)
{
    const log_field_t fields[] = {
        log_u64("free_internal_bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        log_u64("free_psram_bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
    };
    port_log_write(log, LOG_LEVEL_INFO, SLIDESHOW_TAG, "memory after display init", fields,
                   sizeof(fields) / sizeof(fields[0]));
}

static void show_next(slideshow_t *show, port_display_t *display, const port_clock_t *clock,
                      const port_log_t *log)
{
    framebuffer_t *fb = display->get_framebuffer(display);
    if (fb == NULL) {
        port_log_write(log, LOG_LEVEL_ERROR, SLIDESHOW_TAG, "get_framebuffer returned NULL", NULL,
                       0u);
        slideshow_flushed(show, clock->now_ms(clock), false);
        return;
    }

    const size_t index = slideshow_current(show);
    const char *name = slideshow_current_name(show);
    slideshow_render(show, fb);

    const uint64_t start_ms = clock->now_ms(clock);
    const hal_status_t status = display->flush(display, NULL, REFRESH_FULL);
    const uint64_t end_ms = clock->now_ms(clock);
    slideshow_flushed(show, end_ms, status == HAL_OK);

    const log_field_t fields[] = {
        log_u64("index", (uint64_t)index + 1u),
        log_u64("count", SLIDESHOW_IMAGES_COUNT),
        log_str("name", name),
        log_i64("status", (int64_t)status),
        log_u64("flush_ms", end_ms - start_ms),
        log_u64("next_in_ms", slideshow_next_due_ms(show) - end_ms),
    };
    port_log_write(log, (status == HAL_OK) ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR, SLIDESHOW_TAG,
                   "picture shown", fields, sizeof(fields) / sizeof(fields[0]));
}

void app_main(void)
{
    static adp_log_t s_log;
    static adp_clock_t s_clock;
    static adp_display_t s_display;
    static slideshow_t s_show;

    adp_log_init(&s_log);
    const port_log_t *log = adp_log_port(&s_log);
    adp_clock_init(&s_clock, log);
    const port_clock_t *clock = adp_clock_port(&s_clock);

    const hal_status_t display_status =
        adp_display_init(&s_display, BOARD_EPD_WIDTH, BOARD_EPD_HEIGHT, log);
    if (display_status != HAL_OK) {
        const log_field_t fields[] = {log_i64("status", (int64_t)display_status)};
        port_log_write(log, LOG_LEVEL_ERROR, SLIDESHOW_TAG, "adp_display_init failed", fields, 1u);
        return;
    }
    port_display_t *display = adp_display_port(&s_display);
    log_memory(log);

    const slideshow_config_t cfg = {
        .interval_ms = (uint32_t)CONFIG_HAC_SLIDESHOW_INTERVAL_S * 1000u,
        .background = SLIDESHOW_BACKGROUND,
        .caption = SLIDESHOW_CAPTION,
    };
    // The panel's minimum full-refresh interval is a guard, not a measured
    // limit (docs/adr/0008-panel-timing.md), and a comparison run may ask to
    // pass under it (main/Kconfig.projbuild,
    // HAC_SLIDESHOW_OVERRIDE_PANEL_MINIMUM). Said at WARN, so a log never
    // hides that this image drives the panel faster than the product does.
    const uint32_t panel_min_ms =
        SLIDESHOW_OVERRIDE_PANEL_MINIMUM ? 0u : display->min_full_refresh_interval_ms;
    if (SLIDESHOW_OVERRIDE_PANEL_MINIMUM) {
        const log_field_t fields[] = {
            log_u64("panel_min_full_refresh_interval_ms", display->min_full_refresh_interval_ms),
            log_u64("interval_ms", cfg.interval_ms),
        };
        port_log_write(log, LOG_LEVEL_WARN, SLIDESHOW_TAG,
                       "panel minimum full-refresh interval overridden", fields,
                       sizeof(fields) / sizeof(fields[0]));
    }
    slideshow_init(&s_show, slideshow_images_all, slideshow_images_names, SLIDESHOW_IMAGES_COUNT,
                   &cfg, panel_min_ms);

    const log_field_t start_fields[] = {
        log_u64("pictures", SLIDESHOW_IMAGES_COUNT),
        log_u64("pixel_bytes", slideshow_pixel_bytes(&s_show)),
        log_u64("interval_asked_ms", cfg.interval_ms),
        log_u64("interval_ms", slideshow_interval_ms(&s_show)),
        log_bool("caption", SLIDESHOW_CAPTION),
        log_bool("panel_minimum_overridden", SLIDESHOW_OVERRIDE_PANEL_MINIMUM),
    };
    port_log_write(log, LOG_LEVEL_INFO, SLIDESHOW_TAG, "slideshow started", start_fields,
                   sizeof(start_fields) / sizeof(start_fields[0]));

    for (;;) {
        const uint64_t now_ms = clock->now_ms(clock);
        const uint64_t due_ms = slideshow_next_due_ms(&s_show);
        if (now_ms >= due_ms) {
            show_next(&s_show, display, clock, log);
            continue;
        }
        // The interval is capped at a day by Kconfig, so the wait fits.
        clock->delay_ms(clock, (uint32_t)(due_ms - now_ms));
    }
}
