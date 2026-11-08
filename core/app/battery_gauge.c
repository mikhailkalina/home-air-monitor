// SPDX-License-Identifier: Apache-2.0

#include "app/battery_gauge.h"

#include <string.h>

battery_gauge_config_t battery_gauge_config_default(void)
{
    battery_gauge_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.limit_hysteresis_mv = BATTERY_GAUGE_DEFAULT_LIMIT_HYSTERESIS_MV;
    cfg.percent_step = BATTERY_GAUGE_DEFAULT_PERCENT_STEP;
    cfg.sample_interval_ms = BATTERY_GAUGE_DEFAULT_SAMPLE_INTERVAL_MS;
    cfg.max_silence_ms = BATTERY_GAUGE_DEFAULT_MAX_SILENCE_MS;
    return cfg;
}

void battery_gauge_init(battery_gauge_t *g, const battery_gauge_config_t *cfg)
{
    memset(g, 0, sizeof(*g));
    g->cfg = *cfg;
}

uint8_t battery_percent_from_mv(const battery_curve_point_t *curve, size_t points, uint32_t mv)
{
    if (curve == NULL || points == 0u) {
        return 0u;
    }
    if (mv <= curve[0].mv) {
        return curve[0].percent;
    }
    for (size_t i = 1u; i < points; ++i) {
        if (mv <= curve[i].mv) {
            const uint32_t span_mv = (uint32_t)curve[i].mv - curve[i - 1u].mv;
            const uint32_t span_pct = (uint32_t)curve[i].percent - curve[i - 1u].percent;
            if (span_mv == 0u) {
                return curve[i].percent;
            }
            const uint32_t above = mv - curve[i - 1u].mv;
            return (uint8_t)(curve[i - 1u].percent + (above * span_pct + span_mv / 2u) / span_mv);
        }
    }
    return curve[points - 1u].percent;
}

bool battery_gauge_due(const battery_gauge_t *g, uint64_t now_ms)
{
    return !g->have_attempt || now_ms >= g->next_sample_ms ||
           now_ms + g->cfg.sample_interval_ms < g->next_sample_ms;
}

// The window's median. Its first bursts count as far as they go: the first
// number is on the glass with the boot's first frame.
static uint16_t window_median(const battery_gauge_t *g)
{
    uint16_t sorted[BATTERY_GAUGE_WINDOW];
    for (size_t i = 0u; i < g->window_count; ++i) {
        uint16_t v = g->window[i];
        size_t at = i;
        while (at > 0u && sorted[at - 1u] > v) {
            sorted[at] = sorted[at - 1u];
            --at;
        }
        sorted[at] = v;
    }
    return sorted[g->window_count / 2u];
}

// Whether the median is a cell's, staying as it was within the hysteresis of
// either limit.
static bool within_limits(const battery_gauge_t *g, uint16_t mv)
{
    const battery_gauge_config_t *c = &g->cfg;
    const uint32_t low = (uint32_t)c->lowest_cell_mv + (g->in_limits ? 0u : c->limit_hysteresis_mv);
    const uint32_t high_limit = c->highest_cell_mv;
    const uint32_t high = (g->in_limits || high_limit < c->limit_hysteresis_mv)
                              ? high_limit
                              : high_limit - c->limit_hysteresis_mv;
    return mv >= low && mv <= high;
}

static void move_shown(battery_gauge_t *g)
{
    const uint8_t estimate =
        battery_percent_from_mv(g->cfg.curve, g->cfg.curve_points, g->median_mv);
    const uint8_t lowest = (g->cfg.curve_points > 0u) ? g->cfg.curve[0].percent : 0u;
    const uint8_t highest =
        (g->cfg.curve_points > 0u) ? g->cfg.curve[g->cfg.curve_points - 1u].percent : 0u;
    const uint8_t apart = (uint8_t)((estimate > g->shown_percent) ? estimate - g->shown_percent
                                                                  : g->shown_percent - estimate);
    if (!g->have_shown || apart >= g->cfg.percent_step || estimate == lowest ||
        estimate == highest) {
        g->shown_percent = estimate;
        g->have_shown = true;
    }
}

void battery_gauge_sample(battery_gauge_t *g, bool ok, uint32_t mv, uint64_t now_ms)
{
    if (!g->have_attempt) {
        g->silent_from_ms = now_ms;
    }
    g->have_attempt = true;
    g->next_sample_ms = now_ms + g->cfg.sample_interval_ms;

    if (!ok) {
        // A clock that restarted leaves the stamp meaningless: the silence
        // counts from here.
        if (now_ms < g->silent_from_ms) {
            g->silent_from_ms = now_ms;
        }
        if (now_ms - g->silent_from_ms >= g->cfg.max_silence_ms) {
            g->silent = true;
        }
        return;
    }

    g->have_good = true;
    g->silent = false;
    g->silent_from_ms = now_ms;

    g->window[g->window_next] = (mv > UINT16_MAX) ? UINT16_MAX : (uint16_t)mv;
    g->window_next = (g->window_next + 1u) % BATTERY_GAUGE_WINDOW;
    if (g->window_count < BATTERY_GAUGE_WINDOW) {
        g->window_count++;
    }
    g->median_mv = window_median(g);

    const bool first = g->window_count == 1u;
    if (first) {
        // No state to stay in yet: the limits as they are, with no hysteresis.
        g->in_limits = true;
    }
    const bool inside = within_limits(g, g->median_mv);
    if (inside != g->in_limits) {
        // A cell gone or come is another scale: its first number is taken as
        // it is, not stepped towards from an old one.
        g->have_shown = false;
    }
    g->in_limits = inside;
    if (inside) {
        move_shown(g);
    } else {
        g->have_shown = false;
    }
}

battery_gauge_state_t battery_gauge_state(const battery_gauge_t *g)
{
    if (g->silent) {
        return BATTERY_GAUGE_SILENT;
    }
    if (!g->have_good) {
        return BATTERY_GAUGE_NO_SAMPLE;
    }
    return g->in_limits ? BATTERY_GAUGE_CELL : BATTERY_GAUGE_NOT_A_CELL;
}

void battery_gauge_fill(const battery_gauge_t *g, uint64_t now_ms, measurement_f32_t *percent,
                        measurement_f32_t *mv)
{
    const uint8_t id = g->cfg.source_id;
    switch (battery_gauge_state(g)) {
        case BATTERY_GAUGE_CELL:
            *percent = measurement_ok((float)g->shown_percent, now_ms, id);
            *mv = measurement_ok((float)g->median_mv, now_ms, id);
            break;
        case BATTERY_GAUGE_NOT_A_CELL:
            *percent = measurement_status(VAL_UNAVAILABLE, now_ms, id);
            *mv = measurement_ok((float)g->median_mv, now_ms, id);
            break;
        case BATTERY_GAUGE_SILENT:
            *percent = measurement_status(VAL_ERROR, now_ms, id);
            *mv = measurement_status(VAL_ERROR, now_ms, id);
            break;
        case BATTERY_GAUGE_NO_SAMPLE:
        default:
            *percent = measurement_status(VAL_UNAVAILABLE, now_ms, id);
            *mv = measurement_status(VAL_UNAVAILABLE, now_ms, id);
            break;
    }
}

const char *battery_gauge_state_name(battery_gauge_state_t state)
{
    switch (state) {
        case BATTERY_GAUGE_NO_SAMPLE:
            return "no-sample";
        case BATTERY_GAUGE_CELL:
            return "cell";
        case BATTERY_GAUGE_NOT_A_CELL:
            return "not-a-cell";
        case BATTERY_GAUGE_SILENT:
            return "silent";
    }
    return "?";
}
