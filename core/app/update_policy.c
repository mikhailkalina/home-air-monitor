// SPDX-License-Identifier: Apache-2.0

#include "app/update_policy.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

update_policy_config_t update_policy_config_default(void)
{
    update_policy_config_t cfg;

    cfg.co2_delta_ppm = UPDATE_POLICY_DEFAULT_CO2_DELTA_PPM;
    cfg.temperature_delta_c = UPDATE_POLICY_DEFAULT_TEMPERATURE_DELTA_C;
    cfg.humidity_delta_rh = UPDATE_POLICY_DEFAULT_HUMIDITY_DELTA_RH;
    cfg.pressure_delta_hpa = UPDATE_POLICY_DEFAULT_PRESSURE_DELTA_HPA;
    cfg.iaq_delta = UPDATE_POLICY_DEFAULT_IAQ_DELTA;
    cfg.settle_after_ms = UPDATE_POLICY_DEFAULT_SETTLE_AFTER_MS;
    cfg.min_redraw_interval_ms = UPDATE_POLICY_DEFAULT_MIN_REDRAW_INTERVAL_MS;
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

// Whether the screen's accuracy mark would differ (view_model.h): a source's
// verdict crossing measurement_low_accuracy(). A change within one side of it
// -- BSEC's IAQ between medium and high, which on the host it does every few
// minutes -- draws the same and is no reason to spend a partial refresh.
static bool accuracy_marks_differ(const air_reading_t *a, const air_reading_t *b)
{
    const measurement_f32_t *pairs[][2] = {
        {&a->temperature_c, &b->temperature_c},
        {&a->humidity_rh, &b->humidity_rh},
        {&a->pressure_hpa, &b->pressure_hpa},
        {&a->co2_ppm, &b->co2_ppm},
        {&a->iaq, &b->iaq},
        {&a->voc_index, &b->voc_index},
        {&a->gas_resistance_ohm, &b->gas_resistance_ohm},
        {&a->battery_percent, &b->battery_percent},
    };
    for (size_t i = 0u; i < sizeof(pairs) / sizeof(pairs[0]); ++i) {
        if (measurement_low_accuracy(pairs[i][0]) != measurement_low_accuracy(pairs[i][1])) {
            return true;
        }
    }
    return false;
}

// Whether a wall time is known: the footer's "updated --:--" becomes a time
// of day when the clock is first set -- at the console, or in phase 4 by
// SNTP -- and the screen should say so without waiting for a reading to move.
// Only whether, never the time itself, which changes at every poll.
static bool wall_time_known(const air_reading_t *r)
{
    return r->wall_time_ms != 0u;
}

// Every status the home screen renders, plus the link flags in its status bar,
// the accuracy mark and whether the wall time is known. Any of them changing
// must reach the glass even when no number moved: docs/requirements.md 15.1
// requires a sensor error to be visible on screen.
static bool statuses_differ(const air_reading_t *a, const air_reading_t *b)
{
    return a->temperature_c.status != b->temperature_c.status ||
           a->humidity_rh.status != b->humidity_rh.status ||
           a->pressure_hpa.status != b->pressure_hpa.status ||
           a->co2_ppm.status != b->co2_ppm.status || a->iaq.status != b->iaq.status ||
           a->voc_index.status != b->voc_index.status ||
           a->gas_resistance_ohm.status != b->gas_resistance_ohm.status ||
           a->battery_percent.status != b->battery_percent.status || a->charging != b->charging ||
           a->net_connected != b->net_connected ||
           a->telemetry_connected != b->telemetry_connected || accuracy_marks_differ(a, b) ||
           wall_time_known(a) != wall_time_known(b);
}

// --- an offset that persists ------------------------------------------------

// A reading with a delta, beside its frame on the glass.
typedef struct {
    const measurement_f32_t *now;
    const measurement_f32_t *drawn;
    float delta;
} delta_field_t;

// The readings with a delta, in a fixed order: the index of each is its slot
// in offset_held[] and offset_since_ms[].
static void delta_fields(const update_policy_t *p, const air_reading_t *r,
                         delta_field_t out[UPDATE_POLICY_DELTA_FIELDS])
{
    const air_reading_t *f = &p->last_rendered;
    const delta_field_t fields[UPDATE_POLICY_DELTA_FIELDS] = {
        {&r->co2_ppm, &f->co2_ppm, p->cfg.co2_delta_ppm},
        {&r->temperature_c, &f->temperature_c, p->cfg.temperature_delta_c},
        {&r->humidity_rh, &f->humidity_rh, p->cfg.humidity_delta_rh},
        {&r->pressure_hpa, &f->pressure_hpa, p->cfg.pressure_delta_hpa},
        {&r->iaq, &f->iaq, p->cfg.iaq_delta},
    };
    memcpy(out, fields, sizeof(fields));
}

// Notes, per reading with a delta, since when it has been more than
// UPDATE_POLICY_SETTLE_DELTA_FRACTION of its delta from the frame on the
// glass. Without a break: a reading back within it starts over.
static void observe_offsets(update_policy_t *p, const air_reading_t *reading, uint64_t now_ms)
{
    delta_field_t fields[UPDATE_POLICY_DELTA_FIELDS];
    delta_fields(p, reading, fields);
    for (size_t i = 0u; i < UPDATE_POLICY_DELTA_FIELDS; ++i) {
        const bool off = p->have_last_rendered &&
                         delta_exceeded(fields[i].now, fields[i].drawn,
                                        fields[i].delta * UPDATE_POLICY_SETTLE_DELTA_FRACTION);
        if (!off) {
            p->offset_held[i] = false;
        } else if (!p->offset_held[i] || now_ms < p->offset_since_ms[i]) {
            // A new offset -- or the monotonic clock restarted, and the old
            // stamp means nothing.
            p->offset_held[i] = true;
            p->offset_since_ms[i] = now_ms;
        }
    }
}

// When the first of the offsets held will have held settle_after_ms, or
// UPDATE_POLICY_NO_DEADLINE.
static uint64_t settle_at(const update_policy_t *p)
{
    uint64_t at = UPDATE_POLICY_NO_DEADLINE;
    if (p->cfg.settle_after_ms == 0u) {
        return at;
    }
    for (size_t i = 0u; i < UPDATE_POLICY_DELTA_FIELDS; ++i) {
        if (p->offset_held[i]) {
            const uint64_t due = p->offset_since_ms[i] + (uint64_t)p->cfg.settle_after_ms;
            if (due < at) {
                at = due;
            }
        }
    }
    return at;
}

static update_reason_t due_reason(const update_policy_t *p, const air_reading_t *reading,
                                  uint64_t now_ms)
{
    if (!p->have_last_rendered) {
        return UPDATE_REASON_FIRST_FRAME;
    }
    if (p->requested) {
        return UPDATE_REASON_REQUEST;
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
    if (delta_exceeded(&reading->pressure_hpa, &p->last_rendered.pressure_hpa,
                       p->cfg.pressure_delta_hpa)) {
        return UPDATE_REASON_PRESSURE;
    }
    if (delta_exceeded(&reading->iaq, &p->last_rendered.iaq, p->cfg.iaq_delta)) {
        return UPDATE_REASON_IAQ;
    }
    if (statuses_differ(reading, &p->last_rendered)) {
        return UPDATE_REASON_STATUS;
    }
    if (settle_at(p) <= now_ms) {
        return UPDATE_REASON_SETTLE;
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

// What the panel allows for a redraw due for `reason`, at the clock's time.
static void decide(const update_policy_t *p, update_reason_t reason, update_decision_t *out)
{
    const uint64_t now_ms = p->clock->now_ms(p->clock);

    memset(out, 0, sizeof(*out));
    out->action = UPDATE_NONE;
    out->reason = UPDATE_REASON_NONE;
    out->next_deadline_ms = UPDATE_POLICY_NO_DEADLINE;  // nothing changes with time alone

    if (reason == UPDATE_REASON_NONE) {
        return;
    }
    out->reason = reason;

    // One redraw per cycle: one that comes due sooner after the last waits,
    // and takes whatever else changes meanwhile with it. Not the first frame,
    // not a redraw a person asked for, and not across a clock restart, where
    // the old stamp means nothing.
    if (reason != UPDATE_REASON_FIRST_FRAME && reason != UPDATE_REASON_REQUEST &&
        now_ms >= p->last_refresh_ms) {
        const uint64_t redraw_at = p->last_refresh_ms + (uint64_t)p->cfg.min_redraw_interval_ms;
        if (now_ms < redraw_at) {
            out->action = UPDATE_NONE;
            out->held_for_redraw_interval = true;
            out->next_deadline_ms = redraw_at;
            return;
        }
    }

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
        const uint64_t most_partials = (uint64_t)p->limits.max_partial_refreshes_before_full *
                                       UPDATE_POLICY_MOST_PARTIALS_PER_BUDGET;
        if (now_ms < allowed_at) {
            if (p->limits.max_partial_refreshes_before_full > 0u &&
                p->partials_since_full < most_partials) {
                // Past the budget, before a full refresh is allowed: keep the
                // screen current with a partial one. The first redraw that is
                // due once the interval has elapsed is full, since
                // partial_allowed() stays false until a full refresh resets
                // the count. Nothing is redrawn only because the interval
                // elapsed (docs/adr/0019-partials-past-the-budget.md).
                out->action = UPDATE_PARTIAL;
                out->full_forced_by_budget = false;
                out->partial_past_budget = true;
                out->next_deadline_ms = now_ms;
                return;
            }
            // A panel with no partial mode has nothing to keep the screen
            // current with, and one past the budget by the budget again has
            // had what it may take: the redraw waits for the full refresh.
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

void update_policy_evaluate(update_policy_t *p, const air_reading_t *reading,
                            update_decision_t *out)
{
    const uint64_t now_ms = p->clock->now_ms(p->clock);
    observe_offsets(p, reading, now_ms);
    decide(p, due_reason(p, reading, now_ms), out);

    // Nothing due: the next thing that can change the answer without new data
    // is an offset reaching settle_after_ms.
    if (out->reason == UPDATE_REASON_NONE) {
        out->next_deadline_ms = settle_at(p);
    }
}

void update_policy_evaluate_content(const update_policy_t *p, bool changed, update_decision_t *out)
{
    update_reason_t reason = UPDATE_REASON_NONE;
    if (!p->have_last_rendered) {
        reason = UPDATE_REASON_FIRST_FRAME;
    } else if (p->requested) {
        reason = UPDATE_REASON_REQUEST;
    } else if (changed) {
        reason = UPDATE_REASON_STATUS;
    }
    decide(p, reason, out);
}

void update_policy_request(update_policy_t *p)
{
    p->requested = true;
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
    p->requested = false;  // whatever was asked for is what this redraw drew
    // Offsets are measured from this frame now.
    memset(p->offset_held, 0, sizeof(p->offset_held));

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
        case UPDATE_REASON_PRESSURE:
            return "pressure";
        case UPDATE_REASON_IAQ:
            return "iaq";
        case UPDATE_REASON_SETTLE:
            return "settle";
        case UPDATE_REASON_STATUS:
            return "status";
        case UPDATE_REASON_REQUEST:
            return "request";
    }
    return "?";
}
