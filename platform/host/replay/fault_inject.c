// SPDX-License-Identifier: Apache-2.0

#include "fault_inject.h"

#include <string.h>

static fault_inject_t *self_of(sensor_source_t *port)
{
    return (fault_inject_t *)port->impl;
}

static const fault_inject_t *self_of_const(const sensor_source_t *port)
{
    return (const fault_inject_t *)port->impl;
}

static void force_sample_status(sensor_sample_t *s, sensor_caps_t caps, value_status_t status)
{
    // Each field keeps the timestamp and source_id the inner read already
    // gave it: a forced fault still says *when* and *from whom* it did not
    // get a trustworthy number.
    if (caps & SENS_CAP_TEMPERATURE) {
        s->temperature_c =
            measurement_status(status, s->temperature_c.ts_ms, s->temperature_c.source_id);
    }
    if (caps & SENS_CAP_HUMIDITY) {
        s->humidity_rh = measurement_status(status, s->humidity_rh.ts_ms, s->humidity_rh.source_id);
    }
    if (caps & SENS_CAP_PRESSURE) {
        s->pressure_hpa =
            measurement_status(status, s->pressure_hpa.ts_ms, s->pressure_hpa.source_id);
    }
    if (caps & SENS_CAP_CO2) {
        s->co2_ppm = measurement_status(status, s->co2_ppm.ts_ms, s->co2_ppm.source_id);
    }
    if (caps & SENS_CAP_IAQ) {
        s->iaq = measurement_status(status, s->iaq.ts_ms, s->iaq.source_id);
    }
    if (caps & SENS_CAP_VOC) {
        s->voc_index = measurement_status(status, s->voc_index.ts_ms, s->voc_index.source_id);
    }
    if (caps & SENS_CAP_GAS_RES) {
        s->gas_resistance_ohm = measurement_status(status, s->gas_resistance_ohm.ts_ms,
                                                   s->gas_resistance_ohm.source_id);
    }
}

static hal_status_t fi_init(sensor_source_t *self)
{
    fault_inject_t *fi = self_of(self);
    return fi->inner->init(fi->inner);
}

static hal_status_t fi_start(sensor_source_t *self)
{
    fault_inject_t *fi = self_of(self);
    return fi->inner->start(fi->inner);
}

static uint32_t fi_ready_in_ms(const sensor_source_t *self)
{
    const fault_inject_t *fi = self_of_const(self);
    return fi->inner->ready_in_ms(fi->inner);
}

static hal_status_t fi_read(sensor_source_t *self, sensor_sample_t *out)
{
    fault_inject_t *fi = self_of(self);

    if (fi->kind == FAULT_BUS_NACK) {
        return HAL_ERR_IO;  // as if the device NACKed: no I/O succeeded at all
    }
    if (fi->kind == FAULT_BUS_TIMEOUT) {
        return HAL_ERR_TIMEOUT;  // as if the bus stopped answering
    }
    if (fi->kind == FAULT_STUCK && fi->have_last_good) {
        *out = fi->last_good;
        return HAL_OK;
    }

    hal_status_t status = fi->inner->read(fi->inner, out);
    if (status != HAL_OK) {
        return status;
    }

    // Cache every real reading, fault or not, so a later switch to
    // FAULT_STUCK always has something recent to freeze on.
    fi->last_good = *out;
    fi->have_last_good = true;

    if (fi->kind == FAULT_FORCE_STATUS) {
        force_sample_status(out, fi->inner->caps, fi->forced_status);
    }
    return HAL_OK;
}

static hal_status_t fi_suspend(sensor_source_t *self)
{
    fault_inject_t *fi = self_of(self);
    return fi->inner->suspend(fi->inner);
}

static void fi_deinit(sensor_source_t *self)
{
    fault_inject_t *fi = self_of(self);
    fi->inner->deinit(fi->inner);
}

void fault_inject_init(fault_inject_t *fi, sensor_source_t *inner)
{
    memset(fi, 0, sizeof(*fi));
    fi->inner = inner;
    fi->kind = FAULT_NONE;

    fi->port.name = inner->name;
    fi->port.id = inner->id;
    fi->port.caps = inner->caps;
    fi->port.init = fi_init;
    fi->port.start = fi_start;
    fi->port.ready_in_ms = fi_ready_in_ms;
    fi->port.read = fi_read;
    fi->port.suspend = fi_suspend;
    fi->port.deinit = fi_deinit;
    fi->port.impl = fi;
}

void fault_inject_set(fault_inject_t *fi, fault_kind_t kind)
{
    fi->kind = kind;
}

void fault_inject_set_forced_status(fault_inject_t *fi, value_status_t status)
{
    fi->forced_status = status;
}

void fault_inject_clear(fault_inject_t *fi)
{
    fault_inject_set(fi, FAULT_NONE);
}
