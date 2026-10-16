// SPDX-License-Identifier: Apache-2.0

#include "scenario_world.h"

#include <math.h>
#include <string.h>

// PHYSICS: the Magnus formula for the saturation vapour pressure over water,
// e_s(t) = 6.112 hPa * exp(17.62 t / (243.12 + t)), with the coefficients of
// the WMO Guide to Instruments and Methods of Observation (WMO-No. 8). Only
// the ratio at two temperatures is used, so the 6.112 hPa cancels. A second
// statement of what drivers/bme68x/bme68x_limits.h holds, kept apart as the
// models keep theirs: the world is not the driver's to describe.
#define MAGNUS_B 17.62f
#define MAGNUS_C_C 243.12f

#define HUMIDITY_MAX_RH 100.0f

// The scenario's source id is irrelevant here: these samples never leave the
// world.
#define WORLD_SAMPLE_ID 0u

static float saturation_ratio(float t_c)
{
    return expf(MAGNUS_B * t_c / (MAGNUS_C_C + t_c));
}

static bool ok(const measurement_f32_t *m)
{
    return m->status == VAL_OK;
}

// A row that is not "ok" shows in every column the file has as a status
// other than VAL_OK; a column the file lacks is VAL_UNAVAILABLE.
static bool row_failed(const sensor_sample_t *s)
{
    const measurement_f32_t *fields[] = {
        &s->temperature_c, &s->humidity_rh, &s->pressure_hpa,       &s->co2_ppm,
        &s->iaq,           &s->voc_index,   &s->gas_resistance_ohm,
    };
    for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        if (fields[i]->status != VAL_UNAVAILABLE && fields[i]->status != VAL_OK) {
            return true;
        }
    }
    return false;
}

static void sample(const scenario_world_t *w, const replay_timeline_t *tl, bool have,
                   uint64_t at_ms, sensor_sample_t *out)
{
    memset(out, 0, sizeof(*out));  // every field VAL_UNAVAILABLE
    if (have) {
        replay_timeline_sample(tl, at_ms, w->interpolate, at_ms, WORLD_SAMPLE_ID, out);
    }
}

static void scd41_reading(void *ctx, uint64_t at_ms, scd41_model_reading_t *out)
{
    scenario_world_t *w = (scenario_world_t *)ctx;
    sensor_sample_t co2;
    sensor_sample_t env;
    sample(w, &w->co2, w->have_co2, at_ms, &co2);
    sample(w, &w->env, w->have_env, at_ms, &env);

    if (w->have_co2 && row_failed(&co2)) {
        w->scd41_failed_rows++;
        w->scd41->faults.nack_address = 1u;  // the read that would collect this fails
        *out = w->scd41_last;
        return;
    }

    if (ok(&co2.co2_ppm)) {
        w->scd41_last.co2_ppm = co2.co2_ppm.value;
    }
    if (ok(&co2.temperature_c)) {
        w->scd41_last.temperature_c = co2.temperature_c.value;
    } else if (ok(&env.temperature_c)) {
        w->scd41_last.temperature_c = env.temperature_c.value;
    }
    if (ok(&co2.humidity_rh)) {
        w->scd41_last.humidity_rh = co2.humidity_rh.value;
    } else if (ok(&env.humidity_rh)) {
        w->scd41_last.humidity_rh = env.humidity_rh.value;
    }
    *out = w->scd41_last;
}

static void bme680_reading(void *ctx, uint64_t at_ms, bme680_model_reading_t *out)
{
    scenario_world_t *w = (scenario_world_t *)ctx;
    sensor_sample_t env;
    sensor_sample_t co2;
    sample(w, &w->env, w->have_env, at_ms, &env);
    sample(w, &w->co2, w->have_co2, at_ms, &co2);

    if (w->have_env && row_failed(&env)) {
        w->bme680_failed_rows++;
        w->bme680->faults.nack_address = 1u;
        *out = w->bme680_last;
        return;
    }

    // The room, from the environment file, or from the CO2 file where the
    // environment file has nothing.
    const measurement_f32_t *t = ok(&env.temperature_c) ? &env.temperature_c : &co2.temperature_c;
    const measurement_f32_t *rh = ok(&env.humidity_rh) ? &env.humidity_rh : &co2.humidity_rh;
    const measurement_f32_t *p = ok(&env.pressure_hpa) ? &env.pressure_hpa : &co2.pressure_hpa;
    const measurement_f32_t *gas =
        ok(&env.gas_resistance_ohm) ? &env.gas_resistance_ohm : &co2.gas_resistance_ohm;

    if (ok(t)) {
        const float die_c = t->value + w->bme680_self_heating_c;
        w->bme680_last.temperature_c = die_c;
        if (ok(rh)) {
            const float moved = rh->value * saturation_ratio(t->value) / saturation_ratio(die_c);
            w->bme680_last.humidity_rh = (moved < HUMIDITY_MAX_RH) ? moved : HUMIDITY_MAX_RH;
        }
    }
    if (ok(p)) {
        w->bme680_last.pressure_hpa = p->value;
    }
    if (ok(gas)) {
        w->bme680_last.gas_resistance_ohm = gas->value;
    }
    *out = w->bme680_last;
}

hal_status_t scenario_world_init(scenario_world_t *w, const char *co2_path, const char *env_path,
                                 bool interpolate, float bme680_self_heating_c)
{
    memset(w, 0, sizeof(*w));
    if (co2_path == NULL && env_path == NULL) {
        return HAL_ERR_INVALID_ARG;
    }
    w->interpolate = interpolate;
    w->bme680_self_heating_c = bme680_self_heating_c;

    if (co2_path != NULL) {
        const hal_status_t st = replay_parse_file(co2_path, &w->co2);
        if (st != HAL_OK) {
            return st;
        }
        w->have_co2 = true;
    }
    if (env_path != NULL) {
        const hal_status_t st = replay_parse_file(env_path, &w->env);
        if (st != HAL_OK) {
            replay_timeline_dispose(&w->co2);
            w->have_co2 = false;
            return st;
        }
        w->have_env = true;
    }
    return HAL_OK;
}

void scenario_world_attach(scenario_world_t *w, scd41_model_t *scd41, bme680_model_t *bme680)
{
    w->scd41 = scd41;
    w->bme680 = bme680;
    if (scd41 != NULL) {
        w->scd41_last = scd41->default_reading;
        scd41_model_set_reading(scd41, scd41_reading, w);
    }
    if (bme680 != NULL) {
        w->bme680_last = bme680->default_reading;
        bme680_model_set_reading(bme680, bme680_reading, w);
    }
}

void scenario_world_dispose(scenario_world_t *w)
{
    replay_timeline_dispose(&w->co2);
    replay_timeline_dispose(&w->env);
    w->have_co2 = false;
    w->have_env = false;
}
