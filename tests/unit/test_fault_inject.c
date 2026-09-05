// SPDX-License-Identifier: Apache-2.0
//
// fault_inject_t wraps a sensor_source_t and only changes what read() does;
// the "inner" here is a trivial counting stub so the tests stay about
// fault_inject's own logic rather than about a real driver or replay_source.

#include <string.h>

#include "hac_test.h"
#include "replay/fault_inject.h"

#define SRC_ID 7u

typedef struct {
    sensor_source_t port;
    int read_count;
} dummy_source_t;

static hal_status_t dummy_init(sensor_source_t *self)
{
    (void)self;
    return HAL_OK;
}

static hal_status_t dummy_start(sensor_source_t *self)
{
    (void)self;
    return HAL_OK;
}

static uint32_t dummy_ready_in_ms(const sensor_source_t *self)
{
    (void)self;
    return 0u;
}

static hal_status_t dummy_read(sensor_source_t *self, sensor_sample_t *out)
{
    dummy_source_t *d = (dummy_source_t *)self->impl;

    d->read_count++;
    memset(out, 0, sizeof(*out));
    out->co2_ppm =
        measurement_ok(600.0f + (float)d->read_count, 1000u * (uint64_t)d->read_count, self->id);
    return HAL_OK;
}

static hal_status_t dummy_suspend(sensor_source_t *self)
{
    (void)self;
    return HAL_OK;
}

static void dummy_deinit(sensor_source_t *self)
{
    (void)self;
}

static void dummy_source_init(dummy_source_t *d)
{
    memset(d, 0, sizeof(*d));
    d->port.name = "dummy";
    d->port.id = SRC_ID;
    d->port.caps = SENS_CAP_CO2;
    d->port.init = dummy_init;
    d->port.start = dummy_start;
    d->port.ready_in_ms = dummy_ready_in_ms;
    d->port.read = dummy_read;
    d->port.suspend = dummy_suspend;
    d->port.deinit = dummy_deinit;
    d->port.impl = d;
}

static void a_fresh_wrapper_passes_reads_through_unchanged(void)
{
    dummy_source_t dummy;
    dummy_source_init(&dummy);

    fault_inject_t fi;
    fault_inject_init(&fi, &dummy.port);

    sensor_sample_t sample;
    HAC_CHECK_EQ_INT(fi.port.read(&fi.port, &sample), HAL_OK);
    HAC_CHECK_EQ_INT(sample.co2_ppm.status, VAL_OK);
    HAC_CHECK_EQ_F32(sample.co2_ppm.value, 601.0f, 0.01f);
    HAC_CHECK_EQ_INT(dummy.read_count, 1);
}

static void bus_nack_fails_without_touching_the_inner_source(void)
{
    dummy_source_t dummy;
    dummy_source_init(&dummy);

    fault_inject_t fi;
    fault_inject_init(&fi, &dummy.port);
    fault_inject_set(&fi, FAULT_BUS_NACK);

    sensor_sample_t sample;
    HAC_CHECK_EQ_INT(fi.port.read(&fi.port, &sample), HAL_ERR_IO);
    HAC_CHECK_EQ_INT(dummy.read_count, 0);
}

static void bus_timeout_fails_without_touching_the_inner_source(void)
{
    dummy_source_t dummy;
    dummy_source_init(&dummy);

    fault_inject_t fi;
    fault_inject_init(&fi, &dummy.port);
    fault_inject_set(&fi, FAULT_BUS_TIMEOUT);

    sensor_sample_t sample;
    HAC_CHECK_EQ_INT(fi.port.read(&fi.port, &sample), HAL_ERR_TIMEOUT);
    HAC_CHECK_EQ_INT(dummy.read_count, 0);
}

static void stuck_freezes_on_the_last_good_sample(void)
{
    dummy_source_t dummy;
    dummy_source_init(&dummy);

    fault_inject_t fi;
    fault_inject_init(&fi, &dummy.port);

    sensor_sample_t sample;
    HAC_CHECK_EQ_INT(fi.port.read(&fi.port, &sample), HAL_OK);  // seeds last_good, read_count -> 1
    HAC_CHECK_EQ_F32(sample.co2_ppm.value, 601.0f, 0.01f);

    fault_inject_set(&fi, FAULT_STUCK);
    for (int i = 0; i < 3; i++) {
        HAC_CHECK_EQ_INT(fi.port.read(&fi.port, &sample), HAL_OK);
        HAC_CHECK_EQ_F32(sample.co2_ppm.value, 601.0f, 0.01f);  // frozen, not 602/603/604
    }
    HAC_CHECK_EQ_INT(dummy.read_count, 1);  // the inner source was never asked again
}

static void stuck_without_a_prior_read_seeds_itself_from_the_inner_source(void)
{
    dummy_source_t dummy;
    dummy_source_init(&dummy);

    fault_inject_t fi;
    fault_inject_init(&fi, &dummy.port);
    fault_inject_set(&fi, FAULT_STUCK);

    sensor_sample_t sample;
    HAC_CHECK_EQ_INT(fi.port.read(&fi.port, &sample), HAL_OK);
    HAC_CHECK_EQ_F32(sample.co2_ppm.value, 601.0f, 0.01f);
    HAC_CHECK_EQ_INT(dummy.read_count, 1);

    HAC_CHECK_EQ_INT(fi.port.read(&fi.port, &sample), HAL_OK);
    HAC_CHECK_EQ_F32(sample.co2_ppm.value, 601.0f, 0.01f);  // still frozen on the first sample
    HAC_CHECK_EQ_INT(dummy.read_count, 1);
}

static void force_status_overwrites_status_but_keeps_timestamp_and_source(void)
{
    dummy_source_t dummy;
    dummy_source_init(&dummy);

    fault_inject_t fi;
    fault_inject_init(&fi, &dummy.port);
    fault_inject_set(&fi, FAULT_FORCE_STATUS);
    fault_inject_set_forced_status(&fi, VAL_ERROR);

    sensor_sample_t sample;
    HAC_CHECK_EQ_INT(fi.port.read(&fi.port, &sample), HAL_OK);
    HAC_CHECK_EQ_INT(sample.co2_ppm.status, VAL_ERROR);
    HAC_CHECK_EQ_U64(sample.co2_ppm.ts_ms, 1000u);  // preserved from the real reading
    HAC_CHECK_EQ_INT(sample.co2_ppm.source_id, SRC_ID);
    HAC_CHECK_EQ_INT(dummy.read_count, 1);  // the real driver was still asked
}

static void clear_restores_pass_through(void)
{
    dummy_source_t dummy;
    dummy_source_init(&dummy);

    fault_inject_t fi;
    fault_inject_init(&fi, &dummy.port);
    fault_inject_set(&fi, FAULT_BUS_NACK);

    sensor_sample_t sample;
    HAC_CHECK_EQ_INT(fi.port.read(&fi.port, &sample), HAL_ERR_IO);

    fault_inject_clear(&fi);
    HAC_CHECK_EQ_INT(fi.port.read(&fi.port, &sample), HAL_OK);
    HAC_CHECK_EQ_INT(dummy.read_count, 1);
}

static void every_other_call_is_forwarded_to_the_inner_source(void)
{
    dummy_source_t dummy;
    dummy_source_init(&dummy);

    fault_inject_t fi;
    fault_inject_init(&fi, &dummy.port);

    HAC_CHECK_EQ_INT(fi.port.init(&fi.port), HAL_OK);
    HAC_CHECK_EQ_INT(fi.port.start(&fi.port), HAL_OK);
    HAC_CHECK_EQ_U64(fi.port.ready_in_ms(&fi.port), 0u);
    HAC_CHECK_EQ_INT(fi.port.suspend(&fi.port), HAL_OK);
    fi.port.deinit(&fi.port);  // must not crash

    HAC_CHECK_EQ_INT(fi.port.caps, SENS_CAP_CO2);
    HAC_CHECK_EQ_INT(fi.port.id, SRC_ID);
}

int main(void)
{
    HAC_RUN(a_fresh_wrapper_passes_reads_through_unchanged);
    HAC_RUN(bus_nack_fails_without_touching_the_inner_source);
    HAC_RUN(bus_timeout_fails_without_touching_the_inner_source);
    HAC_RUN(stuck_freezes_on_the_last_good_sample);
    HAC_RUN(stuck_without_a_prior_read_seeds_itself_from_the_inner_source);
    HAC_RUN(force_status_overwrites_status_but_keeps_timestamp_and_source);
    HAC_RUN(clear_restores_pass_through);
    HAC_RUN(every_other_call_is_forwarded_to_the_inner_source);
    return hac_test_summary();
}
