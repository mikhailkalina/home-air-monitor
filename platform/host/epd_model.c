// SPDX-License-Identifier: Apache-2.0

#include "epd_model.h"

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

// Interpolates a mode's quoted latency range by the fraction of the panel the
// flush covers: a small update lands near the minimum, a whole-screen one near
// the maximum. Both endpoints are unmeasured estimates (epd_emulation.h).
static uint32_t duration_for(refresh_mode_t mode, uint32_t area_px, uint32_t panel_px)
{
    uint32_t lo;
    uint32_t hi;

    switch (mode) {
        case REFRESH_FULL:
            lo = EPD_SIM_FULL_REFRESH_MIN_MS;
            hi = EPD_SIM_FULL_REFRESH_MAX_MS;
            break;
        case REFRESH_PARTIAL:
            lo = EPD_SIM_PARTIAL_REFRESH_MIN_MS;
            hi = EPD_SIM_PARTIAL_REFRESH_MAX_MS;
            break;
        case REFRESH_FAST_MONO:
            lo = EPD_SIM_FAST_MONO_REFRESH_MIN_MS;
            hi = EPD_SIM_FAST_MONO_REFRESH_MAX_MS;
            break;
        case REFRESH_CLEAR:
        default:
            return EPD_SIM_CLEAR_REFRESH_MS;  // several inversions; area plays no part
    }

    if (panel_px == 0u) {
        return lo;
    }
    const uint64_t span = (uint64_t)(hi - lo);
    return lo + (uint32_t)((span * (uint64_t)area_px) / (uint64_t)panel_px);
}

void epd_model_flush(epd_model_t *m, const rect_t *area, refresh_mode_t mode, epd_flush_t *out)
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

    out->duration_ms = duration_for(out->performed, area_px, panel_px);
    out->partials_since_full = m->partials_since_full;

    m->flush_count++;
    m->simulated_ms_total += out->duration_ms;
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

    printf("[epd] flush %-5u %-16s rect=%u,%u %ux%u (%u%%) %4u ms  partials=%u/%u  panel-time=%.1f "
           "s\n",
           m->flush_count, mode, f->area.x, f->area.y, f->area.w, f->area.h, f->area_percent,
           f->duration_ms, f->partials_since_full, m->max_partial_refreshes_before_full,
           (double)m->simulated_ms_total / 1000.0);

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
