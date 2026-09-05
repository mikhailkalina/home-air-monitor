// SPDX-License-Identifier: Apache-2.0

#include "domain/measurement.h"

#include <math.h>
#include <string.h>

// air_reading_init() and every "no data yet" path rely on this: a zero-filled
// reading must read as unavailable, not as a set of valid zeroes.
_Static_assert(VAL_UNAVAILABLE == 0, "VAL_UNAVAILABLE must be the zero value");

measurement_f32_t measurement_unavailable(void)
{
    measurement_f32_t m;
    memset(&m, 0, sizeof(m));
    return m;
}

measurement_f32_t measurement_ok(float value, uint64_t ts_ms, uint8_t source_id)
{
    measurement_f32_t m = measurement_unavailable();

    m.ts_ms = ts_ms;
    m.source_id = source_id;

    // A NaN or an infinity out of a driver is a fault, not a reading. The
    // value stays at the zero left by measurement_unavailable().
    if (isfinite(value)) {
        m.status = VAL_OK;
        m.value = value;
    } else {
        m.status = VAL_ERROR;
    }
    return m;
}

measurement_f32_t measurement_status(value_status_t status, uint64_t ts_ms, uint8_t source_id)
{
    measurement_f32_t m = measurement_unavailable();

    m.status = status;
    m.ts_ms = ts_ms;
    m.source_id = source_id;
    return m;
}

bool measurement_has_value(const measurement_f32_t *m)
{
    return m->status == VAL_OK || m->status == VAL_STALE;
}

uint64_t measurement_age_ms(const measurement_f32_t *m, uint64_t now_ms)
{
    if (m->status == VAL_UNAVAILABLE) {
        return MEASUREMENT_AGE_UNKNOWN;  // never acquired, so ts_ms means nothing
    }
    if (now_ms < m->ts_ms) {
        // The monotonic clock restarted under us: the timestamp belongs to a
        // previous power cycle and cannot be compared against this one.
        return MEASUREMENT_AGE_UNKNOWN;
    }
    return now_ms - m->ts_ms;
}

bool measurement_is_stale(const measurement_f32_t *m, uint64_t now_ms, uint32_t max_age_ms)
{
    if (!measurement_has_value(m)) {
        return false;
    }
    if (m->status == VAL_STALE) {
        return true;  // already aged out; nothing makes it fresh again
    }
    // MEASUREMENT_AGE_UNKNOWN exceeds every limit, so an unknown age is stale.
    return measurement_age_ms(m, now_ms) > (uint64_t)max_age_ms;
}

void measurement_apply_age(measurement_f32_t *m, uint64_t now_ms, uint32_t max_age_ms)
{
    if (measurement_is_stale(m, now_ms, max_age_ms)) {
        m->status = VAL_STALE;
    }
}

void air_reading_init(air_reading_t *r)
{
    memset(r, 0, sizeof(*r));
}

void air_reading_apply_age(air_reading_t *r, uint64_t now_ms, uint32_t max_age_ms)
{
    measurement_apply_age(&r->temperature_c, now_ms, max_age_ms);
    measurement_apply_age(&r->humidity_rh, now_ms, max_age_ms);
    measurement_apply_age(&r->pressure_hpa, now_ms, max_age_ms);
    measurement_apply_age(&r->co2_ppm, now_ms, max_age_ms);
    measurement_apply_age(&r->iaq, now_ms, max_age_ms);
    measurement_apply_age(&r->voc_index, now_ms, max_age_ms);
    measurement_apply_age(&r->gas_resistance_ohm, now_ms, max_age_ms);
    measurement_apply_age(&r->battery_percent, now_ms, max_age_ms);
    measurement_apply_age(&r->battery_mv, now_ms, max_age_ms);
}
