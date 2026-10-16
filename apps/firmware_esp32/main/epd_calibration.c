// SPDX-License-Identifier: Apache-2.0

#include "epd_calibration.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "board_config.h"
#include "domain/civil_time.h"
#include "domain/measurement.h"
#include "hal_status.h"
#include "ui/fonts/font_dejavu_sans.h"
#include "ui/framebuffer.h"
#include "ui/gfx.h"
#include "ui/screen_home.h"
#include "ui/view_model.h"

#include "duration_stats.h"

#define CAL_TAG "epd_cal"

// The main-task stack this mode needs, enforced at build time because the
// first calibration image overflowed the default 3584 bytes on its first
// flush. Measured from that image's own prologues (`entry a1, N` in the
// disassembly), the deepest chain -- app_main -> epd_calibration_run (448)
// -> run_ghost_probe (384) -> timed_flush (512) -> adp_flush (336) -> the
// ESP_LOG path down to _vfprintf_r (800) and the USB-Serial-JTAG write --
// is about 3.25 KB of frames before the VFS layer and an interrupt's spill
// area. 8 KB leaves more than half of it spare. sdkconfig.calibration sets
// it; a calibration build configured any other way fails here rather than
// on the panel.
#define CAL_MIN_MAIN_TASK_STACK_BYTES 8192

#if CONFIG_HAC_EPD_CALIBRATION && (CONFIG_ESP_MAIN_TASK_STACK_SIZE < CAL_MIN_MAIN_TASK_STACK_BYTES)
#error "HAC_EPD_CALIBRATION needs ESP_MAIN_TASK_STACK_SIZE >= 8192 (use sdkconfig.calibration)"
#endif

// The two GRAY4 levels, and the only two REFRESH_FAST_MONO can reach
// (platform/esp32_t5s3/adp_display.c's mode mapping).
#define CAL_BLACK 0x0u
#define CAL_WHITE 0xFu

// --- timing shapes ---------------------------------------------------------
//
// EPDiy's row-by-row output clocks out every one of the panel's rows on every
// waveform pass, writing the rows that hold a changed pixel and skipping the
// rest (output_i2s/render_i2s.c, i2s_output_frame()); a written row is the
// whole panel width whatever the rectangle's width. If that reading of the
// source is right, a partial refresh costs by height, not by area -- which is
// not what the simulator assumes. The shapes are chosen to test exactly that:
// three heights at a narrow width, and one height at two widths.
//
// All anchored at the origin. Every pair of drives fills the shape solid and
// then clears it again, so every row inside it is dirty on both drives and
// the panel returns to white between cases.
#define CAL_NARROW_W 240u
#define CAL_SHORT_H 60u
// The height of the CO2 headline's dirty rectangle in the phase-2b log
// (docs/hardware/board_notes.md, "Measured with EPDiy"), so one case times
// the shape the product actually refreshes most.
#define CAL_HEADLINE_H 228u

// --- ghosting probe --------------------------------------------------------
//
// EPD_CAL_GHOST_SAWTOOTH: the headline walks the way phase 2's synthetic
// bring-up data (bringup_reading.c, replaced by the real sensors in phase 3c)
// made it walk under update_policy: one step just past the 50 ppm redraw
// threshold per partial
// refresh, rising then falling, crossing the Good/Moderate/Poor boundaries
// (and so inverting the level banner) on the way. The realistic case, not the
// worst one.
#define CAL_GHOST_CO2_START_PPM 600.0f
#define CAL_GHOST_CO2_MAX_PPM 1620.0f
#define CAL_GHOST_CO2_STEP_PPM 60.0f

// EPD_CAL_GHOST_SHIFT: the headline alternates between a three-digit and a
// four-digit value on every step, so the "ppm" unit jumps one digit width
// back and forth across ground the digits just left. The first sawtooth run
// saw remnants only there, and only at its first 4->3 digit change (step 28),
// which cannot tell "28 partials accumulate a visible residue" from "a
// layout shift leaves one at any count"; this pattern shifts every step and
// separates the two. Both values sit in the Moderate band (view_model.h's
// VM_CO2_GOOD_MAX_PPM .. VM_CO2_MODERATE_MAX_PPM), so the level banner never
// changes and is not part of what is observed.
#define CAL_SHIFT_NARROW_PPM 980.0f
#define CAL_SHIFT_WIDE_PPM 1040.0f
#define CAL_GHOST_TEMPERATURE_C 22.0f
#define CAL_GHOST_HUMIDITY_RH 45.0f
#define CAL_CO2_SOURCE_ID 1u
#define CAL_ENV_SOURCE_ID 2u

// The step counter drawn on the panel, so the observer can name the step
// without reading the serial log. Placed between the CO2 unit mark and the
// level banner, on the baseline of the CO2 value (core/ui/screen_home.c's
// HEAD_VALUE_BASELINE): rows the headline already dirties on every step, so
// it adds no dirty rows and does not change what is being timed. Coupled to
// screen_home's layout by hand -- if that layout moves, move this.
#define CAL_COUNTER_X 440
#define CAL_COUNTER_BASELINE 180

// --- grey ramp -------------------------------------------------------------
#define CAL_RAMP_LEVELS 16u
#define CAL_RAMP_LABEL_INSET 24  // label baseline above the bottom edge
#define CAL_RAMP_LIGHT_FROM 8u   // levels from here up get a black label

// Which ramp this is, drawn near the top over the light bars (black ink there
// reads), so a photograph of the panel identifies itself.
#define CAL_RAMP_CAPTION_X 560
#define CAL_RAMP_CAPTION_BASELINE 60

#define CAL_NAME_MAX 32u

typedef enum {
    CAL_CLEAR = 0,
    CAL_FULL_BLACK,
    CAL_FULL_HOME,
    CAL_FULL_RAMP,
    CAL_PARTIAL_PANEL,
    CAL_PARTIAL_WIDE_HEADLINE,
    CAL_PARTIAL_HEADLINE,
    CAL_PARTIAL_SHORT,
    CAL_MONO_PANEL,
    CAL_MONO_HEADLINE,
    CAL_MONO_SHORT,
    CAL_GHOST_PARTIAL,
    CAL_CASE_COUNT,
} cal_case_id_t;

// The cases the timing batch drives in pairs, in this order.
#define CAL_BATCH_FIRST CAL_PARTIAL_PANEL
#define CAL_BATCH_LAST CAL_MONO_SHORT
#define CAL_DRIVES_PER_PAIR 2u

typedef struct {
    char name[CAL_NAME_MAX];
    refresh_mode_t mode;
    bool whole_panel;
    rect_t rect;  // meaningful only when !whole_panel; 0x0 for the probe's varying rects
    duration_stats_t stats;
    uint32_t failures;
} cal_case_t;

typedef struct {
    port_display_t *display;
    const port_clock_t *clock;
    const port_log_t *log;
    framebuffer_t *fb;

    cal_case_t cases[CAL_CASE_COUNT];

    bool have_whole;
    uint64_t last_whole_ms;
    uint32_t partials_since_whole;
} cal_t;

// Static rather than on app_main's stack: see epd_calibration.h.
static cal_t s_cal;
static air_reading_t s_reading;
static view_model_t s_vm;

static const char *mode_name(refresh_mode_t mode)
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

static rect_t make_rect(uint16_t w, uint16_t h)
{
    const rect_t r = {0u, 0u, w, h};
    return r;
}

static void define_case(cal_t *c, cal_case_id_t id, const char *name, refresh_mode_t mode,
                        bool whole_panel, rect_t rect)
{
    cal_case_t *k = &c->cases[id];

    memset(k, 0, sizeof(*k));
    (void)snprintf(k->name, sizeof(k->name), "%s", name);
    k->mode = mode;
    k->whole_panel = whole_panel;
    k->rect = rect;
    duration_stats_init(&k->stats);
}

static void define_cases(cal_t *c)
{
    const uint16_t w = c->display->width;
    const uint16_t h = c->display->height;
    const rect_t none = make_rect(0u, 0u);

    define_case(c, CAL_CLEAR, "clear", REFRESH_CLEAR, true, none);
    define_case(c, CAL_FULL_BLACK, "full_black", REFRESH_FULL, true, none);
    define_case(c, CAL_FULL_HOME, "full_home", REFRESH_FULL, true, none);
    define_case(c, CAL_FULL_RAMP, "full_ramp", REFRESH_FULL, true, none);
    define_case(c, CAL_PARTIAL_PANEL, "partial_panel", REFRESH_PARTIAL, false, make_rect(w, h));
    define_case(c, CAL_PARTIAL_WIDE_HEADLINE, "partial_wide_headline", REFRESH_PARTIAL, false,
                make_rect(w, CAL_HEADLINE_H));
    define_case(c, CAL_PARTIAL_HEADLINE, "partial_headline", REFRESH_PARTIAL, false,
                make_rect(CAL_NARROW_W, CAL_HEADLINE_H));
    define_case(c, CAL_PARTIAL_SHORT, "partial_short", REFRESH_PARTIAL, false,
                make_rect(CAL_NARROW_W, CAL_SHORT_H));
    define_case(c, CAL_MONO_PANEL, "fast_mono_panel", REFRESH_FAST_MONO, false, make_rect(w, h));
    define_case(c, CAL_MONO_HEADLINE, "fast_mono_headline", REFRESH_FAST_MONO, false,
                make_rect(CAL_NARROW_W, CAL_HEADLINE_H));
    define_case(c, CAL_MONO_SHORT, "fast_mono_short", REFRESH_FAST_MONO, false,
                make_rect(CAL_NARROW_W, CAL_SHORT_H));
    define_case(c, CAL_GHOST_PARTIAL, "ghost_probe_partial", REFRESH_PARTIAL, false, none);
}

// --- drives ----------------------------------------------------------------

// The one place a drive happens. Times flush() with esp_timer, attributes the
// sample (or the failure) to `id`, and keeps the whole-panel and
// partial-budget bookkeeping the spacing rules below rely on.
static hal_status_t timed_flush(cal_t *c, cal_case_id_t id, const rect_t *area)
{
    cal_case_t *k = &c->cases[id];

    const int64_t start_us = esp_timer_get_time();
    const hal_status_t status = c->display->flush(c->display, area, k->mode);
    const int64_t elapsed_us = esp_timer_get_time() - start_us;

    if (status == HAL_OK && elapsed_us >= 0) {
        duration_stats_add(&k->stats, (uint64_t)elapsed_us);
    } else {
        k->failures++;
    }

    if (k->whole_panel) {
        c->have_whole = true;
        c->last_whole_ms = c->clock->now_ms(c->clock);
        c->partials_since_whole = 0u;
    } else {
        c->partials_since_whole++;
    }

    const rect_t shown = (area != NULL) ? *area : make_rect(c->display->width, c->display->height);
    const log_field_t fields[] = {
        log_str("case", k->name),
        log_str("mode", mode_name(k->mode)),
        log_u64("x", shown.x),
        log_u64("y", shown.y),
        log_u64("w", shown.w),
        log_u64("h", shown.h),
        log_i64("elapsed_us", elapsed_us),
        log_i64("status", (int64_t)status),
        log_u64("partials_since_whole", c->partials_since_whole),
    };
    port_log_write(c->log, (status == HAL_OK) ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR, CAL_TAG, "sample",
                   fields, sizeof(fields) / sizeof(fields[0]));
    return status;
}

// Waits until the panel's reported minimum interval since the last
// whole-panel drive has elapsed. The same rule main.c's test pattern and
// core/app/update_policy.c follow: the interval is what this adapter tells
// the core the panel tolerates, so a diagnostic does not get to ignore it.
static void wait_for_whole_panel_slot(cal_t *c)
{
    if (!c->have_whole) {
        return;
    }

    const uint64_t due_ms = c->last_whole_ms + (uint64_t)c->display->min_full_refresh_interval_ms;
    const uint64_t now_ms = c->clock->now_ms(c->clock);
    if (now_ms >= due_ms) {
        return;
    }

    const uint64_t wait_ms = due_ms - now_ms;
    const log_field_t fields[] = {log_u64("wait_ms", wait_ms)};
    port_log_write(c->log, LOG_LEVEL_INFO, CAL_TAG,
                   "waiting out the panel's minimum full-refresh interval", fields, 1u);
    c->clock->delay_ms(c->clock, (uint32_t)wait_ms);
}

static void whole_panel_flush(cal_t *c, cal_case_id_t id)
{
    wait_for_whole_panel_slot(c);
    (void)timed_flush(c, id, NULL);
}

// --- content ---------------------------------------------------------------

static void render_home(cal_t *c, float co2_ppm)
{
    const uint64_t now_ms = c->clock->now_ms(c->clock);

    air_reading_init(&s_reading);
    s_reading.co2_ppm = measurement_ok(co2_ppm, now_ms, CAL_CO2_SOURCE_ID);
    s_reading.temperature_c = measurement_ok(CAL_GHOST_TEMPERATURE_C, now_ms, CAL_ENV_SOURCE_ID);
    s_reading.humidity_rh = measurement_ok(CAL_GHOST_HUMIDITY_RH, now_ms, CAL_ENV_SOURCE_ID);

    // No wall time in this reading: the footer says "updated --:--", and the
    // zone is never read.
    const time_zone_t utc = time_zone_fixed(0);
    view_model_build(&s_vm, &s_reading, now_ms, &utc);
    screen_home_render(c->fb, &s_vm);
}

static void draw_counter(cal_t *c, const char *text)
{
    (void)gfx_draw_text(c->fb, CAL_COUNTER_X, CAL_COUNTER_BASELINE, &font_dejavu_sans_32, text,
                        CAL_BLACK);
}

static void render_grey_ramp(cal_t *c, const char *caption)
{
    const int32_t w = (int32_t)c->fb->width;
    const int32_t h = (int32_t)c->fb->height;
    const int32_t bar_w = w / (int32_t)CAL_RAMP_LEVELS;

    for (uint32_t level = 0u; level < CAL_RAMP_LEVELS; ++level) {
        const int32_t x = (int32_t)level * bar_w;
        // The last bar absorbs any remainder, so the ramp always reaches the edge.
        const int32_t this_w = (level + 1u == CAL_RAMP_LEVELS) ? (w - x) : bar_w;
        gfx_fill_rect(c->fb, x, 0, this_w, h, (uint8_t)level);

        char label[4];
        (void)snprintf(label, sizeof(label), "%u", (unsigned)level);
        const int32_t text_w = gfx_measure_text(&font_dejavu_sans_32, label);
        const uint8_t ink = (level >= CAL_RAMP_LIGHT_FROM) ? CAL_BLACK : CAL_WHITE;
        (void)gfx_draw_text(c->fb, x + (this_w - text_w) / 2, h - CAL_RAMP_LABEL_INSET,
                            &font_dejavu_sans_32, label, ink);
    }

    (void)gfx_draw_text(c->fb, CAL_RAMP_CAPTION_X, CAL_RAMP_CAPTION_BASELINE, &font_dejavu_sans_32,
                        caption, CAL_BLACK);
}

// A ramp as its own whole-panel event, spaced like any other.
static void show_ramp(cal_t *c, const char *caption)
{
    render_grey_ramp(c, caption);
    whole_panel_flush(c, CAL_FULL_RAMP);
    const log_field_t fields[] = {log_str("caption", caption)};
    port_log_write(c->log, LOG_LEVEL_WARN, CAL_TAG, "grey ramp on the panel", fields, 1u);
}

// --- residue test ----------------------------------------------------------
//
// Both ghosting probes left residue that survived the full refreshes after
// them: the "ppm" of the shift probe stayed legible on the dark bars of a
// ramp drawn two full refreshes later, and the first run's timing shapes left
// bands the same way. port_display.h defines REFRESH_FULL as the refresh that
// clears accumulated ghosting, and update_policy's partial budget only
// protects the panel if that holds. This answers two questions in order:
//
//   1. does the residue fade under ordinary full refreshes (one epd_clear()
//      of three cycles, then GC16), repeated at the panel's minimum interval?
//   2. does a deeper clean remove what ordinary ones leave -- extra
//      REFRESH_CLEARs back to back immediately before a full refresh, which
//      is exactly what a REFRESH_FULL with more clear cycles would do?
//
// Each step ends on a captioned ramp, because the residue shows on the dark
// greys and not on white.
static void run_residue_test(cal_t *c, uint32_t normal_fulls, uint32_t deep_events,
                             uint32_t extra_clears)
{
    char caption[CAL_NAME_MAX];

    for (uint32_t i = 1u; i <= normal_fulls; ++i) {
        (void)snprintf(caption, sizeof(caption), "normal %u/%u", (unsigned)i,
                       (unsigned)normal_fulls);
        show_ramp(c, caption);
    }

    for (uint32_t j = 1u; j <= deep_events; ++j) {
        // One deep event: the extra clears and the ramp's own full refresh
        // run back to back, as a single deeper REFRESH_FULL would, so the
        // minimum interval is waited out once, before the event.
        wait_for_whole_panel_slot(c);
        for (uint32_t k = 0u; k < extra_clears; ++k) {
            (void)timed_flush(c, CAL_CLEAR, NULL);
        }
        (void)snprintf(caption, sizeof(caption), "deep %u/%u +%u clr", (unsigned)j,
                       (unsigned)deep_events, (unsigned)extra_clears);
        render_grey_ramp(c, caption);
        (void)timed_flush(c, CAL_FULL_RAMP, NULL);
        const log_field_t fields[] = {
            log_str("caption", caption),
            log_u64("extra_clears", extra_clears),
        };
        port_log_write(c->log, LOG_LEVEL_WARN, CAL_TAG, "grey ramp on the panel", fields,
                       sizeof(fields) / sizeof(fields[0]));
    }
}

// --- 1. ghosting probe -----------------------------------------------------

static float first_co2(epd_calibration_ghost_pattern_t pattern)
{
    return (pattern == EPD_CAL_GHOST_SHIFT) ? CAL_SHIFT_NARROW_PPM : CAL_GHOST_CO2_START_PPM;
}

// The value shown at probe step `k` (1-based), given the one shown before it.
static float next_co2(epd_calibration_ghost_pattern_t pattern, uint32_t k, float co2, float *step)
{
    if (pattern == EPD_CAL_GHOST_SHIFT) {
        return ((k % 2u) == 1u) ? CAL_SHIFT_WIDE_PPM : CAL_SHIFT_NARROW_PPM;
    }

    const float next = co2 + *step;
    if (next > CAL_GHOST_CO2_MAX_PPM || next < CAL_GHOST_CO2_START_PPM) {
        *step = -*step;
    }
    return co2 + *step;
}

static void run_ghost_probe(cal_t *c, epd_calibration_ghost_pattern_t pattern, uint32_t partials,
                            uint32_t pause_ms)
{
    if (partials == 0u) {
        port_log_write(c->log, LOG_LEVEL_INFO, CAL_TAG, "ghost probe skipped (0 partials)", NULL,
                       0u);
        return;
    }

    // What the panel last showed, so each step flushes only the rectangle
    // that changed -- exactly as apps/firmware_esp32/main/event_loop.c does,
    // so the probe's rectangles and timings are the product's.
    uint8_t *previous_pixels =
        heap_caps_malloc(c->fb->pixels_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (previous_pixels == NULL) {
        port_log_write(c->log, LOG_LEVEL_ERROR, CAL_TAG,
                       "ghost probe skipped: no PSRAM for the previous-frame buffer", NULL, 0u);
        return;
    }
    framebuffer_t previous;
    framebuffer_init(&previous, previous_pixels, c->fb->pixels_size, c->fb->width, c->fb->height,
                     c->fb->format);

    const log_field_t intro_fields[] = {
        log_str("pattern", (pattern == EPD_CAL_GHOST_SHIFT) ? "shift" : "sawtooth"),
        log_u64("partials", partials),
        log_u64("pause_ms", pause_ms),
        log_u64("panel_budget", c->display->max_partial_refreshes_before_full),
    };
    // Kept short: adp_log formats message and fields into one 256-byte line,
    // and a longer message here cut the fields off in the first run.
    port_log_write(c->log, LOG_LEVEL_WARN, CAL_TAG,
                   "GHOST PROBE: watch the CO2 headline; note the step (beside the value) where "
                   "earlier digits first show through, and if the closing full refresh clears them",
                   intro_fields, sizeof(intro_fields) / sizeof(intro_fields[0]));

    float co2 = first_co2(pattern);
    float step = CAL_GHOST_CO2_STEP_PPM;
    char counter[CAL_NAME_MAX];

    render_home(c, co2);
    draw_counter(c, "#0");
    whole_panel_flush(c, CAL_FULL_HOME);
    memcpy(previous.pixels, c->fb->pixels, c->fb->pixels_size);

    for (uint32_t k = 1u; k <= partials; ++k) {
        c->clock->delay_ms(c->clock, pause_ms);

        co2 = next_co2(pattern, k, co2, &step);
        render_home(c, co2);
        (void)snprintf(counter, sizeof(counter), "#%u", (unsigned)k);
        draw_counter(c, counter);

        rect_t dirty;
        if (!framebuffer_diff_dirty_rect(c->fb, &previous, &dirty)) {
            // Cannot happen while the counter changes every step; logged
            // rather than assumed so a layout change that hides the counter
            // does not silently shorten the probe.
            port_log_write(c->log, LOG_LEVEL_ERROR, CAL_TAG, "ghost probe: empty diff", NULL, 0u);
            continue;
        }

        const log_field_t step_fields[] = {
            log_u64("step", k),
            log_f32("co2_ppm", co2),
        };
        port_log_write(c->log, LOG_LEVEL_WARN, CAL_TAG, "ghost probe step", step_fields,
                       sizeof(step_fields) / sizeof(step_fields[0]));

        (void)timed_flush(c, CAL_GHOST_PARTIAL, &dirty);
        memcpy(previous.pixels, c->fb->pixels, c->fb->pixels_size);
    }

    c->clock->delay_ms(c->clock, pause_ms);
    render_home(c, co2);
    draw_counter(c, "full");
    wait_for_whole_panel_slot(c);
    port_log_write(c->log, LOG_LEVEL_WARN, CAL_TAG,
                   "GHOST PROBE: closing full refresh now -- check that the remnants are gone",
                   NULL, 0u);
    (void)timed_flush(c, CAL_FULL_HOME, NULL);

    heap_caps_free(previous_pixels);
}

// --- 2. timing -------------------------------------------------------------

// Starts a fresh whole-panel window when the next `drives` partial refreshes
// would overrun the panel's budget. REFRESH_CLEAR leaves the glass white,
// which is the state every pair below returns it to anyway, and its time
// counts towards the clear case like any other.
static bool make_room_for(cal_t *c, uint32_t drives)
{
    const uint32_t budget = c->display->max_partial_refreshes_before_full;
    if (budget < drives) {
        return false;
    }
    if (c->partials_since_whole + drives > budget) {
        whole_panel_flush(c, CAL_CLEAR);
    }
    return true;
}

static void run_pair(cal_t *c, cal_case_id_t id)
{
    const cal_case_t *k = &c->cases[id];

    if (!make_room_for(c, CAL_DRIVES_PER_PAIR)) {
        const log_field_t fields[] = {log_str("case", k->name)};
        port_log_write(c->log, LOG_LEVEL_WARN, CAL_TAG,
                       "skipped: the panel's partial budget is smaller than one pair", fields, 1u);
        return;
    }

    const rect_t r = k->rect;
    gfx_fill_rect(c->fb, r.x, r.y, r.w, r.h, CAL_BLACK);
    (void)timed_flush(c, id, &r);
    gfx_fill_rect(c->fb, r.x, r.y, r.w, r.h, CAL_WHITE);
    (void)timed_flush(c, id, &r);
}

static void run_timing(cal_t *c, uint32_t iterations)
{
    for (uint32_t i = 0u; i < iterations; ++i) {
        const log_field_t fields[] = {
            log_u64("iteration", i + 1u),
            log_u64("of", iterations),
        };
        port_log_write(c->log, LOG_LEVEL_INFO, CAL_TAG, "timing iteration", fields,
                       sizeof(fields) / sizeof(fields[0]));

        // REFRESH_CLEAR leaves the glass and EPDiy's model white but the
        // framebuffer alone (adp_display.c); making the framebuffer white too
        // is what lets every pair below drive its whole shape.
        whole_panel_flush(c, CAL_CLEAR);
        framebuffer_clear(c->fb, CAL_WHITE);

        for (int id = CAL_BATCH_FIRST; id <= CAL_BATCH_LAST; ++id) {
            run_pair(c, (cal_case_id_t)id);
        }

        framebuffer_clear(c->fb, CAL_BLACK);
        whole_panel_flush(c, CAL_FULL_BLACK);

        render_home(c, CAL_GHOST_CO2_START_PPM);
        whole_panel_flush(c, CAL_FULL_HOME);
    }
}

// --- reporting -------------------------------------------------------------

static void log_results(const cal_t *c)
{
    for (int id = 0; id < (int)CAL_CASE_COUNT; ++id) {
        const cal_case_t *k = &c->cases[id];
        if (k->stats.count == 0u && k->failures == 0u) {
            continue;
        }

        const uint16_t w = k->whole_panel ? c->display->width : k->rect.w;
        const uint16_t h = k->whole_panel ? c->display->height : k->rect.h;
        const log_field_t fields[] = {
            log_str("case", k->name),
            log_str("mode", mode_name(k->mode)),
            log_u64("w", w),
            log_u64("h", h),
            log_u64("n", k->stats.count),
            log_u64("failures", k->failures),
            log_u64("min_us", k->stats.min_us),
            log_u64("max_us", k->stats.max_us),
            log_u64("mean_us", duration_stats_mean_us(&k->stats)),
        };
        port_log_write(c->log, LOG_LEVEL_WARN, CAL_TAG, "result", fields,
                       sizeof(fields) / sizeof(fields[0]));
    }
}

// --- entry -----------------------------------------------------------------

void epd_calibration_run(port_display_t *display, const port_clock_t *clock, const port_log_t *log,
                         const epd_calibration_config_t *cfg)
{
    cal_t *c = &s_cal;

    memset(c, 0, sizeof(*c));
    c->display = display;
    c->clock = clock;
    c->log = log;
    c->fb = display->get_framebuffer(display);
    if (c->fb == NULL) {
        port_log_write(log, LOG_LEVEL_ERROR, CAL_TAG, "no framebuffer; calibration not run", NULL,
                       0u);
        return;
    }
    define_cases(c);

    // Three whole-panel drives per timing iteration, two for the probe, two
    // for the baseline and closing ramps and one per residue-test step, each
    // preceded by up to one minimum interval.
    const uint64_t interval_ms = display->min_full_refresh_interval_ms;
    const uint64_t whole_events =
        (uint64_t)cfg->iterations * 3u + 4u + cfg->residue_normal_fulls + cfg->residue_deep_events;
    const uint64_t estimate_s =
        ((uint64_t)cfg->ghost_partials * cfg->ghost_pause_ms + whole_events * interval_ms) / 1000u;

    const log_field_t fields[] = {
        log_str("board", BOARD_SILKSCREEN_REVISION),
        log_i64("assumed_temperature_c", BOARD_EPD_ASSUMED_TEMPERATURE_C),
        log_u64("iterations", cfg->iterations),
        log_u64("ghost_partials", cfg->ghost_partials),
        log_u64("ghost_pause_ms", cfg->ghost_pause_ms),
        log_u64("min_full_refresh_interval_ms", interval_ms),
        log_u64("max_partial_refreshes_before_full", display->max_partial_refreshes_before_full),
        log_u64("estimated_duration_s", estimate_s),
    };
    port_log_write(log, LOG_LEVEL_WARN, CAL_TAG, "panel calibration starting", fields,
                   sizeof(fields) / sizeof(fields[0]));

    // A baseline ramp first. The first full calibration run left faint bands
    // across the dark bars of its closing ramp, on the rows its partial and
    // fast-mono drives had covered, through three full refreshes. Mid-dark
    // greys show residue that white hides; a ramp before and after this run
    // shows what earlier runs left and what this one adds.
    show_ramp(c, "baseline");

    run_ghost_probe(c, cfg->ghost_pattern, cfg->ghost_partials, cfg->ghost_pause_ms);
    run_timing(c, cfg->iterations);
    run_residue_test(c, cfg->residue_normal_fulls, cfg->residue_deep_events,
                     cfg->residue_extra_clears);

    show_ramp(c, "closing");

    log_results(c);
    port_log_write(log, LOG_LEVEL_WARN, CAL_TAG, "panel calibration complete", NULL, 0u);
}
