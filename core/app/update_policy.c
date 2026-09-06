// SPDX-License-Identifier: Apache-2.0

#include "app/update_policy.h"

#include <math.h>
#include <string.h>

update_policy_config_t update_policy_config_default(void)
{
    update_policy_config_t cfg;

    cfg.co2_delta_ppm = UPDATE_POLICY_DEFAULT_CO2_DELTA_PPM;
    cfg.temperature_delta_c = UPDATE_POLICY_DEFAULT_TEMPERATURE_DELTA_C;
    cfg.humidity_delta_rh = UPDATE_POLICY_DEFAULT_HUMIDITY_DELTA_RH;
    cfg.max_interval_ms = UPDATE_POLICY_DEFAULT_MAX_INTERVAL_MS;
    return cfg;
}

update_policy_limits_t update_policy_limits_from_display(const port_display_t *display)
{
    update_policy_limits_t limits;

    if (display == NULL) {
        // No panel to ask: permit no partial refreshes and no minimum wait.
        // Erring towards full refreshes costs power; erring the other way
        // damages the glass.
        limits.min_full_refresh_interval_ms = 0u;
        limits.max_partial_refreshes_before_full = 0u;
        return limits;
    }

    limits.min_full_refresh_interval_ms = display->min_full_refresh_interval_ms;
    limits.max_partial_refreshes_before_full = display->max_partial_refreshes_before_full;
    return limits;
}

void update_policy_init(update_policy_t *p, const port_clock_t *clock,
                        const update_policy_config_t *cfg, const update_policy_limits_t *limits)
{
    memset(p, 0, sizeof(*p));

    p->clock = clock;
    p->cfg = *cfg;
    p->limits = *limits;
}

// --- triggers --------------------------------------------------------------

// True when both readings carry a usable number and they differ by strictly
// more than `threshold`. A quantity that gained or lost its value is not a
// delta but a status change, and is caught by statuses_differ().
static bool delta_exceeded(const measurement_f32_t *now, const measurement_f32_t *prev,
                           float threshold)
{
    if (!measurement_has_value(now) || !measurement_has_value(prev)) {
        return false;
    }
    return fabsf(now->value - prev->value) > threshold;
}

// Every status the home screen renders, plus the link flags in its status bar.
// Any of them changing must reach the glass even when no number moved:
// docs/requirements.md 15.1 requires a sensor error to be visible on screen.
static bool statuses_differ(const air_reading_t *a, const air_reading_t *b)
{
    return a->temperature_c.status != b->temperature_c.status ||
           a->humidity_rh.status != b->humidity_rh.status ||
           a->pressure_hpa.status != b->pressure_hpa.status ||
           a->co2_ppm.status != b->co2_ppm.status || a->iaq.status != b->iaq.status ||
           a->voc_index.status != b->voc_index.status ||
           a->gas_resistance_ohm.status != b->gas_resistance_ohm.status ||
           a->battery_percent.status != b->battery_percent.status || a->charging != b->charging ||
           a->net_connected != b->net_connected || a->telemetry_connected != b->telemetry_connected;
}

// The deadline for the periodic redraw, or UPDATE_POLICY_NO_DEADLINE when the
// periodic redraw is switched off.
static uint64_t periodic_deadline(const update_policy_t *p)
{
    if (p->cfg.max_interval_ms == 0u) {
        return UPDATE_POLICY_NO_DEADLINE;
    }
    return p->last_refresh_ms + (uint64_t)p->cfg.max_interval_ms;
}

static bool periodic_due(const update_policy_t *p, uint64_t now_ms)
{
    if (p->cfg.max_interval_ms == 0u) {
        return false;
    }
    if (now_ms < p->last_refresh_ms) {
        return true;  // the monotonic clock restarted: treat the frame as old
    }
    return now_ms >= periodic_deadline(p);
}

static update_reason_t due_reason(const update_policy_t *p, const air_reading_t *reading,
                                  uint64_t now_ms)
{
    if (!p->have_last_rendered) {
        return UPDATE_REASON_FIRST_FRAME;
    }
    if (delta_exceeded(&reading->co2_ppm, &p->last_rendered.co2_ppm, p->cfg.co2_delta_ppm)) {
        return UPDATE_REASON_CO2;
    }
    if (delta_exceeded(&reading->temperature_c, &p->last_rendered.temperature_c,
                       p->cfg.temperature_delta_c)) {
        return UPDATE_REASON_TEMPERATURE;
    }
    if (delta_exceeded(&reading->humidity_rh, &p->last_rendered.humidity_rh,
                       p->cfg.humidity_delta_rh)) {
        return UPDATE_REASON_HUMIDITY;
    }
    if (statuses_differ(reading, &p->last_rendered)) {
        return UPDATE_REASON_STATUS;
    }
    if (periodic_due(p, now_ms)) {
        return UPDATE_REASON_PERIODIC;
    }
    return UPDATE_REASON_NONE;
}

// --- panel constraints -----------------------------------------------------

static bool partial_allowed(const update_policy_t *p)
{
    if (p->limits.max_partial_refreshes_before_full == 0u) {
        return false;  // the panel offers no usable partial mode
    }
    return p->partials_since_full < p->limits.max_partial_refreshes_before_full;
}

// The earliest time at which a full refresh may be issued.
static uint64_t full_allowed_at(const update_policy_t *p, uint64_t now_ms)
{
    if (!p->have_full_refresh) {
        return now_ms;  // never refreshed fully: nothing to wait for
    }
    if (now_ms < p->last_full_refresh_ms) {
        return now_ms;  // the monotonic clock restarted; the old stamp is meaningless
    }
    return p->last_full_refresh_ms + (uint64_t)p->limits.min_full_refresh_interval_ms;
}

// --- evaluation ------------------------------------------------------------

void update_policy_evaluate(const update_policy_t *p, const air_reading_t *reading,
                            update_decision_t *out)
{
    const uint64_t now_ms = p->clock->now_ms(p->clock);

    memset(out, 0, sizeof(*out));
    out->action = UPDATE_NONE;
    out->reason = UPDATE_REASON_NONE;
    out->next_deadline_ms = periodic_deadline(p);

    const update_reason_t reason = due_reason(p, reading, now_ms);
    if (reason == UPDATE_REASON_NONE) {
        return;
    }
    out->reason = reason;

    // A due redraw is partial unless the panel says otherwise. The first frame
    // is always full: there is no previous image for a partial refresh to
    // build on.
    bool want_full = (reason == UPDATE_REASON_FIRST_FRAME);
    if (!want_full && !partial_allowed(p)) {
        want_full = true;
        out->full_forced_by_budget = true;
    }

    if (want_full) {
        const uint64_t allowed_at = full_allowed_at(p, now_ms);
        if (now_ms < allowed_at) {
            // Deferred, not downgraded. Continuing to refresh partially past
            // the panel's budget is what leaves residual images and damages
            // the glass, so waiting is the only safe answer.
            out->action = UPDATE_NONE;
            out->deferred_by_min_interval = true;
            out->next_deadline_ms = allowed_at;
            return;
        }
        out->action = UPDATE_FULL;
    } else {
        out->action = UPDATE_PARTIAL;
    }

    out->next_deadline_ms = now_ms;  // due now
}

void update_policy_commit(update_policy_t *p, const air_reading_t *rendered,
                          const update_decision_t *decision)
{
    if (decision->action == UPDATE_NONE) {
        return;
    }

    const uint64_t now_ms = p->clock->now_ms(p->clock);

    p->last_rendered = *rendered;
    p->have_last_rendered = true;
    p->last_refresh_ms = now_ms;

    if (decision->action == UPDATE_FULL) {
        p->last_full_refresh_ms = now_ms;
        p->have_full_refresh = true;
        p->partials_since_full = 0u;
    } else {
        p->partials_since_full++;
    }
}

uint32_t update_policy_partials_since_full(const update_policy_t *p)
{
    return p->partials_since_full;
}

const char *update_policy_action_name(update_action_t action)
{
    switch (action) {
        case UPDATE_NONE:
            return "none";
        case UPDATE_PARTIAL:
            return "partial";
        case UPDATE_FULL:
            return "full";
    }
    return "?";
}

const char *update_policy_reason_name(update_reason_t reason)
{
    switch (reason) {
        case UPDATE_REASON_NONE:
            return "none";
        case UPDATE_REASON_FIRST_FRAME:
            return "first-frame";
        case UPDATE_REASON_CO2:
            return "co2";
        case UPDATE_REASON_TEMPERATURE:
            return "temperature";
        case UPDATE_REASON_HUMIDITY:
            return "humidity";
        case UPDATE_REASON_STATUS:
            return "status";
        case UPDATE_REASON_PERIODIC:
            return "periodic";
    }
    return "?";
}
