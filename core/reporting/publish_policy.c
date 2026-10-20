// SPDX-License-Identifier: Apache-2.0

#include "reporting/publish_policy.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "reporting/report_limits.h"

publish_policy_config_t publish_policy_config_default(void)
{
    publish_policy_config_t cfg;
    cfg.co2_delta_ppm = REPORT_CO2_DELTA_PPM;
    cfg.temperature_delta_c = REPORT_TEMPERATURE_DELTA_C;
    cfg.humidity_delta_rh = REPORT_HUMIDITY_DELTA_RH;
    cfg.pressure_delta_hpa = REPORT_PRESSURE_DELTA_HPA;
    cfg.iaq_delta = REPORT_IAQ_DELTA;
    cfg.heartbeat_ms = REPORT_HEARTBEAT_MS;
    cfg.min_interval_ms = REPORT_MIN_INTERVAL_MS;
    return cfg;
}

void publish_policy_init(publish_policy_t *p, const publish_policy_config_t *cfg)
{
    memset(p, 0, sizeof(*p));
    p->cfg = *cfg;
    if (p->cfg.heartbeat_ms == 0u) {
        p->cfg.heartbeat_ms = 1u;
    }
}

void publish_policy_reset(publish_policy_t *p)
{
    p->have_last = false;
}

// Both carry a number and they differ by strictly more than `delta`. A value
// that gained or lost its number is a status change, caught first.
static bool moved(const measurement_f32_t *now, const measurement_f32_t *last, float delta)
{
    if (!measurement_has_value(now) || !measurement_has_value(last)) {
        return false;
    }
    return fabsf(now->value - last->value) > delta;
}

// Every status the state message carries, and IAQ's accuracy, which it
// carries as a word of its own -- any step of it, not only the screen's
// question mark (measurement_low_accuracy()).
static bool statuses_differ(const air_reading_t *a, const air_reading_t *b)
{
    return a->temperature_c.status != b->temperature_c.status ||
           a->humidity_rh.status != b->humidity_rh.status ||
           a->pressure_hpa.status != b->pressure_hpa.status ||
           a->co2_ppm.status != b->co2_ppm.status || a->iaq.status != b->iaq.status ||
           a->gas_resistance_ohm.status != b->gas_resistance_ohm.status ||
           a->iaq.accuracy != b->iaq.accuracy;
}

static publish_reason_t due_reason(const publish_policy_t *p, const air_reading_t *r,
                                   uint64_t now_ms)
{
    if (!p->have_last) {
        return PUBLISH_REASON_SESSION;
    }
    const air_reading_t *l = &p->last;
    if (statuses_differ(r, l)) {
        return PUBLISH_REASON_STATUS;
    }
    if (moved(&r->co2_ppm, &l->co2_ppm, p->cfg.co2_delta_ppm)) {
        return PUBLISH_REASON_CO2;
    }
    if (moved(&r->temperature_c, &l->temperature_c, p->cfg.temperature_delta_c)) {
        return PUBLISH_REASON_TEMPERATURE;
    }
    if (moved(&r->humidity_rh, &l->humidity_rh, p->cfg.humidity_delta_rh)) {
        return PUBLISH_REASON_HUMIDITY;
    }
    if (moved(&r->pressure_hpa, &l->pressure_hpa, p->cfg.pressure_delta_hpa)) {
        return PUBLISH_REASON_PRESSURE;
    }
    if (moved(&r->iaq, &l->iaq, p->cfg.iaq_delta)) {
        return PUBLISH_REASON_IAQ;
    }
    // The clock restarted (deep sleep is a cold boot): the old stamp means
    // nothing, and the heartbeat is due.
    if (now_ms < p->last_ms || now_ms - p->last_ms >= (uint64_t)p->cfg.heartbeat_ms) {
        return PUBLISH_REASON_HEARTBEAT;
    }
    return PUBLISH_REASON_NONE;
}

void publish_policy_evaluate(const publish_policy_t *p, const air_reading_t *reading,
                             uint64_t now_ms, publish_decision_t *out)
{
    memset(out, 0, sizeof(*out));
    out->reason = due_reason(p, reading, now_ms);

    if (out->reason == PUBLISH_REASON_NONE) {
        out->next_deadline_ms = p->last_ms + (uint64_t)p->cfg.heartbeat_ms;
        return;
    }
    // One message per cycle, the session's first excepted, and not across a
    // clock restart.
    if (out->reason != PUBLISH_REASON_SESSION && now_ms >= p->last_ms) {
        const uint64_t allowed_at = p->last_ms + (uint64_t)p->cfg.min_interval_ms;
        if (now_ms < allowed_at) {
            out->held_for_interval = true;
            out->next_deadline_ms = allowed_at;
            return;
        }
    }
    out->due = true;
    out->next_deadline_ms = now_ms;
}

void publish_policy_commit(publish_policy_t *p, const air_reading_t *published, uint64_t now_ms)
{
    p->last = *published;
    p->have_last = true;
    p->last_ms = now_ms;
}

const char *publish_policy_reason_name(publish_reason_t reason)
{
    switch (reason) {
        case PUBLISH_REASON_NONE:
            return "none";
        case PUBLISH_REASON_SESSION:
            return "session";
        case PUBLISH_REASON_STATUS:
            return "status";
        case PUBLISH_REASON_CO2:
            return "co2";
        case PUBLISH_REASON_TEMPERATURE:
            return "temperature";
        case PUBLISH_REASON_HUMIDITY:
            return "humidity";
        case PUBLISH_REASON_PRESSURE:
            return "pressure";
        case PUBLISH_REASON_IAQ:
            return "iaq";
        case PUBLISH_REASON_HEARTBEAT:
            return "heartbeat";
        case PUBLISH_REASON_COUNT:
            break;
    }
    return "?";
}
