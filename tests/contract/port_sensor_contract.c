// SPDX-License-Identifier: Apache-2.0

#include "port_sensor_contract.h"

#include <stdbool.h>
#include <stddef.h>

static void check(contract_report_fn report, void *ctx, unsigned *failures, const char *name,
                  bool ok)
{
    report(ctx, name, ok);
    if (!ok) {
        (*failures)++;
    }
}

// One field of a sample and the capability bit that would allow it.
typedef struct {
    const measurement_f32_t *m;
    sensor_caps_t cap;
} field_t;

static size_t fields_of(const sensor_sample_t *s, field_t *out)
{
    size_t n = 0;
    out[n++] = (field_t){&s->temperature_c, SENS_CAP_TEMPERATURE};
    out[n++] = (field_t){&s->humidity_rh, SENS_CAP_HUMIDITY};
    out[n++] = (field_t){&s->pressure_hpa, SENS_CAP_PRESSURE};
    out[n++] = (field_t){&s->co2_ppm, SENS_CAP_CO2};
    out[n++] = (field_t){&s->iaq, SENS_CAP_IAQ};
    out[n++] = (field_t){&s->voc_index, SENS_CAP_VOC};
    out[n++] = (field_t){&s->gas_resistance_ohm, SENS_CAP_GAS_RES};
    return n;
}

#define SAMPLE_FIELDS 7u

// A source must never report data for a quantity it did not declare.
static bool fields_outside_caps_are_unavailable(const sensor_sample_t *s, sensor_caps_t caps)
{
    field_t fields[SAMPLE_FIELDS];
    const size_t n = fields_of(s, fields);
    for (size_t i = 0; i < n; i++) {
        if ((caps & fields[i].cap) == 0 && fields[i].m->status != VAL_UNAVAILABLE) {
            return false;
        }
    }
    return true;
}

// Everything a source does report carries the source's id, so the sensor
// manager can tell where a value came from (architecture 4.1).
static bool reported_fields_carry_the_source_id(const sensor_sample_t *s, uint8_t id)
{
    field_t fields[SAMPLE_FIELDS];
    const size_t n = fields_of(s, fields);
    for (size_t i = 0; i < n; i++) {
        if (fields[i].m->status != VAL_UNAVAILABLE && fields[i].m->source_id != id) {
            return false;
        }
    }
    return true;
}

unsigned port_sensor_contract_run(sensor_source_t *src, const port_sensor_contract_env_t *env,
                                  contract_report_fn report, void *report_ctx)
{
    unsigned failures = 0;

    check(report, report_ctx, &failures, "sensor.name_present", src->name != NULL);
    check(report, report_ctx, &failures, "sensor.caps_not_empty", src->caps != 0);
    check(report, report_ctx, &failures, "sensor.init_ok", src->init(src) == HAL_OK);

    sensor_sample_t sample;
    check(report, report_ctx, &failures, "sensor.read_before_start_not_ready",
          src->read(src, &sample) == HAL_ERR_NOT_READY);

    check(report, report_ctx, &failures, "sensor.start_ok", src->start(src) == HAL_OK);

    hal_status_t status = HAL_ERR_NOT_READY;
    for (unsigned attempt = 0; attempt < PORT_SENSOR_CONTRACT_READ_ATTEMPTS && status != HAL_OK;
         attempt++) {
        const uint32_t wait_ms = src->ready_in_ms(src);
        env->advance(env->ctx, wait_ms > 0u ? wait_ms : 1u);
        status = src->read(src, &sample);
    }
    check(report, report_ctx, &failures, "sensor.read_after_ready_in_ms_ok", status == HAL_OK);
    if (status == HAL_OK) {
        check(report, report_ctx, &failures, "sensor.fields_outside_caps_unavailable",
              fields_outside_caps_are_unavailable(&sample, src->caps));
        check(report, report_ctx, &failures, "sensor.reported_fields_carry_source_id",
              reported_fields_carry_the_source_id(&sample, src->id));
    }

    check(report, report_ctx, &failures, "sensor.suspend_ok", src->suspend(src) == HAL_OK);
    src->deinit(src);  // must not crash; the source is unusable after this
    return failures;
}
