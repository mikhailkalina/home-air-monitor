// SPDX-License-Identifier: Apache-2.0

#include "epd_model.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "epd_emulation.h"

void epd_model_init(epd_model_t *m, uint16_t width, uint16_t height,
                    uint32_t max_partial_refreshes_before_full)
{
    memset(m, 0, sizeof(*m));

    m->width = width;
    m->height = height;
    m->max_partial_refreshes_before_full = max_partial_refreshes_before_full;
}

rect_t epd_model_clip(const epd_model_t *m, const rect_t *area)
{
    rect_t r;

    if (area == NULL) {
        r.x = 0u;
        r.y = 0u;
        r.w = m->width;
        r.h = m->height;
        return r;
    }

    r = *area;
    if (r.x >= m->width || r.y >= m->height) {
        r.x = 0u;
        r.y = 0u;
        r.w = 0u;
        r.h = 0u;
        return r;
    }
    if ((uint32_t)r.x + (uint32_t)r.w > (uint32_t)m->width) {
        r.w = (uint16_t)(m->width - r.x);
    }
    if ((uint32_t)r.y + (uint32_t)r.h > (uint32_t)m->height) {
        r.h = (uint16_t)(m->height - r.y);
    }
    return r;
}

// Microseconds to whole milliseconds, rounding half up.
static uint32_t us_to_ms(uint64_t us)
{
    return (uint32_t)((us + 500u) / 1000u);
}

// How long the panel takes for `mode` over a rectangle `area_rows` tall whose
// passes write `written` rows, `changed` of them holding a changed pixel. The
// difference pass covers every row of the rectangle; when nothing changed,
// EPDiy stops there and draws nothing. A full refresh is the clear plus GC16,
// which costs what GL16 does over the same rows (same 30 passes; 1,204.7 ms
// measured for 540 rows against 1,206.1). Every constant is measured
// (epd_emulation.h).
static uint32_t duration_for(refresh_mode_t mode, uint32_t area_rows, uint32_t written,
                             uint32_t changed)
{
    const uint64_t diff_us = (uint64_t)area_rows * EPD_SIM_DIFF_PER_ROW_US;

    switch (mode) {
        case REFRESH_PARTIAL:
        case REFRESH_FULL: {
            uint64_t us = diff_us;
            if (changed > 0u) {
                us += EPD_SIM_PARTIAL_BASE_US + (uint64_t)written * EPD_SIM_PARTIAL_PER_ROW_US;
            }
            const uint32_t ms = us_to_ms(us);
            return (mode == REFRESH_FULL) ? EPD_SIM_CLEAR_REFRESH_MS + ms : ms;
        }
        case REFRESH_FAST_MONO: {
            uint64_t us = diff_us;
            if (changed > 0u) {
                us += EPD_SIM_FAST_MONO_BASE_US + (uint64_t)written * EPD_SIM_FAST_MONO_PER_ROW_US;
            }
            return us_to_ms(us);
        }
        case REFRESH_CLEAR:
        default:
            return EPD_SIM_CLEAR_REFRESH_MS;
    }
}

// Whether row `y` holds a pixel the drive changes, by the rule
// epd_model_flush() documents. With no content to look at, every row of the
// rectangle counts.
static bool row_changes(const epd_model_t *m, bool full, const rect_t *r, uint32_t y,
                        const framebuffer_t *next, const uint8_t *glass)
{
    if (y < r->y || y >= (uint32_t)r->y + r->h) {
        return false;
    }
    if (next == NULL || glass == NULL) {
        return true;
    }
    for (uint32_t x = r->x; x < (uint32_t)r->x + r->w; ++x) {
        const uint8_t level = framebuffer_get_pixel(next, (int32_t)x, (int32_t)y);
        const uint8_t was = full ? EPD_MODEL_GRAY4_WHITE : glass[(size_t)y * m->width + x];
        if (level != was) {
            return true;
        }
    }
    return false;
}

// The rows a pass writes out in full, which is most of a refresh's time. EPDiy's row output
// (output_i2s/render_i2s.c, i2s_output_frame()) visits every panel row on every pass. A row with a
// changed pixel is written. Of each run of unchanged rows, the first two are written too -- as
// no-op rows while the output pipeline drains (i2s_skip_row()'s `skipping < 2`) -- and the rest are
// only clocked past, cheaply. A frame that ends on a written row latches one more out. Counting
// this way, rather than counting changed rows, is what brings screen_home's full refresh, with its
// many separate bands of text, within 1 % of the panel.
static uint32_t written_rows(const epd_model_t *m, refresh_mode_t performed, const rect_t *area,
                             const framebuffer_t *next, const uint8_t *glass, uint32_t *changed_out)
{
    *changed_out = 0u;
    if (performed == REFRESH_CLEAR) {
        return 0u;  // a clear is pushes, not passes; its time is a constant
    }

    const bool full = (performed == REFRESH_FULL);
    const rect_t r = full ? epd_model_clip(m, NULL) : *area;

    uint32_t written = 0u;
    uint32_t changed = 0u;
    uint32_t skipping = 0u;
    for (uint32_t y = 0u; y < m->height; ++y) {
        if (row_changes(m, full, &r, y, next, glass)) {
            written++;
            changed++;
            skipping = 0u;
        } else {
            if (skipping < 2u) {
                written++;
            }
            skipping++;
        }
    }
    if (skipping == 0u) {
        written++;
    }

    // Nothing changed: EPDiy returns before drawing anything at all.
    *changed_out = changed;
    return (changed == 0u) ? 0u : written;
}

void epd_model_flush(epd_model_t *m, const rect_t *area, refresh_mode_t mode,
                     const framebuffer_t *next, const uint8_t *glass, epd_flush_t *out)
{
    memset(out, 0, sizeof(*out));

    out->area = epd_model_clip(m, area);
    out->requested = mode;
    out->performed = mode;

    const uint32_t panel_px = (uint32_t)m->width * (uint32_t)m->height;
    const uint32_t area_px = (uint32_t)out->area.w * (uint32_t)out->area.h;
    out->area_percent =
        (panel_px == 0u) ? 0u : (uint32_t)(((uint64_t)area_px * 100u + panel_px / 2u) / panel_px);

    // The panel protects itself. A caller that keeps asking for partial
    // refreshes past the budget gets a full one anyway -- which is what the
    // hardware would need, and what a correct update_policy would have asked
    // for on its own. Reaching this branch is a finding, not a feature.
    const bool partial_mode = (mode == REFRESH_PARTIAL || mode == REFRESH_FAST_MONO);
    if (partial_mode && m->partials_since_full >= m->max_partial_refreshes_before_full) {
        out->performed = REFRESH_FULL;
        out->forced_full = true;
        m->forced_full_count++;
    }

    if (out->performed == REFRESH_FULL || out->performed == REFRESH_CLEAR) {
        m->partials_since_full = 0u;
    } else {
        m->partials_since_full++;
    }

    if (out->performed == REFRESH_FULL || out->performed == REFRESH_CLEAR) {
        // The device drives the whole panel for both, whatever was asked for
        // (platform/esp32_t5s3/adp_display.c).
        out->area = epd_model_clip(m, NULL);
        out->area_percent = 100u;
    }
    out->written_rows =
        written_rows(m, out->performed, &out->area, next, glass, &out->changed_rows);
    out->duration_ms =
        duration_for(out->performed, out->area.h, out->written_rows, out->changed_rows);
    out->partials_since_full = m->partials_since_full;

    m->flush_count++;
    m->simulated_ms_total += out->duration_ms;
}

uint8_t epd_level_display_grey(uint8_t level)
{
    static const uint16_t k_lightness_permille[16] = EPD_SIM_LEVEL_LIGHTNESS_PERMILLE;
    const uint8_t index = (level < 16u) ? level : 15u;
    const float light = (float)k_lightness_permille[index] / 1000.0f;

    // sRGB's own transfer function: what makes a screen's grey look as light
    // as the panel's.
    const float encoded =
        (light <= 0.0031308f) ? 12.92f * light : 1.055f * powf(light, 1.0f / 2.4f) - 0.055f;
    const float grey = encoded * 255.0f + 0.5f;
    return (uint8_t)((grey >= 255.0f) ? 255.0f : grey);
}

const char *epd_refresh_mode_name(refresh_mode_t mode)
{
    switch (mode) {
        case REFRESH_FULL:
            return "full";
        case REFRESH_PARTIAL:
            return "partial";
        case REFRESH_FAST_MONO:
            return "fast-mono";
        case REFRESH_CLEAR:
            return "clear";
    }
    return "?";
}

void epd_model_log(const epd_model_t *m, const epd_flush_t *f)
{
    char mode[32];
    if (f->forced_full) {
        snprintf(mode, sizeof(mode), "%s->%s", epd_refresh_mode_name(f->requested),
                 epd_refresh_mode_name(f->performed));
    } else {
        snprintf(mode, sizeof(mode), "%s", epd_refresh_mode_name(f->performed));
    }

    printf("[epd] flush %-5u %-16s rect=%u,%u %ux%u (%u%%) rows=%u/%u %4u ms  partials=%u/%u  "
           "panel-time=%.1f s\n",
           m->flush_count, mode, f->area.x, f->area.y, f->area.w, f->area.h, f->area_percent,
           f->changed_rows, f->written_rows, f->duration_ms, f->partials_since_full,
           m->max_partial_refreshes_before_full, (double)m->simulated_ms_total / 1000.0);

    if (f->forced_full) {
        printf("[epd] FORCED FULL REFRESH: the partial-refresh budget of %u was exhausted, so the "
               "panel refreshed fully on its own.\n",
               m->max_partial_refreshes_before_full);
        printf("[epd]   The vendor warns that prolonged partial refreshing leaves residual images "
               "and damages this panel irreversibly\n"
               "[epd]   (docs/hardware/board_notes.md). Reaching this line means the refresh "
               "strategy did not respect max_partial_refreshes_before_full;\n"
               "[epd]   forced full refreshes so far: %u.\n",
               m->forced_full_count);
    }
}

void epd_model_log_summary(const epd_model_t *m)
{
    printf("[epd] summary: %u flushes, %.1f s spent refreshing, %u partial(s) since the last "
           "full refresh\n",
           m->flush_count, (double)m->simulated_ms_total / 1000.0, m->partials_since_full);

    if (m->forced_full_count == 0u) {
        printf("[epd] summary: no forced full refreshes -- the refresh strategy stayed inside\n"
               "[epd]          the panel budget of %u\n",
               m->max_partial_refreshes_before_full);
    } else {
        printf(
            "[epd] summary: %u FORCED full refresh(es). On the real panel this is the failure\n"
            "[epd]          mode the vendor warns about: the refresh strategy must respect\n"
            "[epd]          max_partial_refreshes_before_full instead of relying on the panel.\n",
            m->forced_full_count);
    }
}
