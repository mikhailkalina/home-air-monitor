// SPDX-License-Identifier: Apache-2.0
//
// Host runner for port_sensor_contract.c, the shared body, against every
// sensor_source_t this build has: replay_source (CSV playback);
// scd4x_source -- Sensirion's own driver -- over the fake I2C bus with the
// SCD41 command-level model on it, in both of its modes; and bme68x_source
// -- Bosch's own driver -- over the same bus with the BME680 register-level
// model. Each prints one "contract port_sensor <implementation> <check>
// PASS|FAIL" line per check.

#ifndef HAC_TEST_TMP_DIR
#error "HAC_TEST_TMP_DIR must be defined by the build (see tests/contract/CMakeLists.txt)"
#endif

#include "port_sensor_contract.h"

#include <stdio.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_i2c_fake.h"
#include "bme68x_source.h"
#include "contract_print.h"
#include "hac_test.h"
#include "i2c_models/bme680_model.h"
#include "i2c_models/scd41_model.h"
#include "replay/replay_source.h"
#include "scd4x_source.h"

// The SCD4x's one address (datasheet Table 8). The fake bus knows nothing
// about the board; this is the sensor's, not board_config.h's.
#define SCD41_ADDR 0x62u
#define SCD41_SOURCE_ID 1u

// One of the BME680's two addresses (its datasheet, 6.2), and again the
// sensor's, not the board's.
#define BME680_ADDR 0x77u
#define BME680_SOURCE_ID 2u

static void report_to_hac_test(void *ctx, const char *check, bool ok)
{
    contract_print_check((const contract_print_ctx_t *)ctx, check, ok);
    hac_test_check(ok, check, __FILE__, __LINE__);
}

static void advance_virtual_clock(void *ctx, uint64_t ms)
{
    adp_clock_virtual_advance((adp_clock_virtual_t *)ctx, ms);
}

static char *write_temp_csv(const char *name, const char *content)
{
    static char path[512];
    snprintf(path, sizeof(path), "%s/%s", HAC_TEST_TMP_DIR, name);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return NULL;
    }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return path;
}

static void run(sensor_source_t *src, adp_clock_virtual_t *clock, const char *implementation)
{
    const port_sensor_contract_env_t env = {.advance = advance_virtual_clock, .ctx = clock};
    contract_print_ctx_t ctx = {.port = "port_sensor", .implementation = implementation};
    const unsigned failures = port_sensor_contract_run(src, &env, report_to_hac_test, &ctx);
    HAC_CHECK_EQ_INT((int)failures, 0);
}

static void replay_source_satisfies_the_sensor_source_contract(void)
{
    char *path =
        write_temp_csv("contract_full.csv", "t_offset_s,temperature_c,humidity_rh,co2_ppm,status\n"
                                            "0,21.5,45.0,620,ok\n"
                                            "60,22.0,46.0,650,ok\n");
    HAC_CHECK(path != NULL);

    adp_clock_virtual_t vc;
    adp_clock_virtual_init(&vc, 1.0);

    replay_source_t rs;
    sensor_caps_t caps = SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY | SENS_CAP_CO2;
    HAC_CHECK_EQ_INT(replay_source_init(&rs, path, caps, 1u, adp_clock_virtual_port(&vc), true),
                     HAL_OK);

    run(&rs.port, &vc, "replay_source");
}

static void a_co2_only_replay_source_reports_nothing_outside_its_capability(void)
{
    char *path = write_temp_csv("contract_co2_only.csv", "t_offset_s,co2_ppm,status\n0,600,ok\n");
    HAC_CHECK(path != NULL);

    adp_clock_virtual_t vc;
    adp_clock_virtual_init(&vc, 1.0);

    replay_source_t rs;
    HAC_CHECK_EQ_INT(
        replay_source_init(&rs, path, SENS_CAP_CO2, 2u, adp_clock_virtual_port(&vc), true), HAL_OK);

    run(&rs.port, &vc, "replay_source(co2_only)");
}

typedef struct {
    adp_clock_virtual_t clock;
    adp_i2c_fake_t bus;
    scd41_model_t sensor;
    scd4x_source_t source;
} scd4x_rig_t;

static void scd4x_rig_init(scd4x_rig_t *r, scd4x_mode_t mode)
{
    adp_clock_virtual_init(&r->clock, 1.0);
    adp_i2c_fake_init(&r->bus, &r->clock);
    scd41_model_init(&r->sensor, r->clock.now_ms);
    HAC_CHECK(adp_i2c_fake_attach(&r->bus, SCD41_ADDR, scd41_model_port(&r->sensor)));

    const scd4x_source_config_t cfg = {
        .bus = adp_i2c_fake_port(&r->bus),
        .clock = adp_clock_virtual_port(&r->clock),
        .log = NULL,
        .address = SCD41_ADDR,
        .id = SCD41_SOURCE_ID,
        .mode = mode,
    };
    HAC_CHECK_EQ_INT(scd4x_source_setup(&r->source, &cfg), HAL_OK);
}

static void scd4x_source_satisfies_the_sensor_source_contract_in_periodic_mode(void)
{
    static scd4x_rig_t rig;
    scd4x_rig_init(&rig, SCD4X_MODE_PERIODIC);
    run(&rig.source.port, &rig.clock, "scd4x_source(periodic)+scd41_model");

    // Over the real protocol: nothing was refused along the way.
    HAC_CHECK_EQ_INT((int)rig.sensor.stats.rejected, 0);
    HAC_CHECK_EQ_INT((int)rig.sensor.stats.nacked_while_busy, 0);
    HAC_CHECK(rig.sensor.state == SCD41_MODEL_IDLE);  // suspend() stopped it
}

static void scd4x_source_satisfies_the_sensor_source_contract_in_single_shot_mode(void)
{
    static scd4x_rig_t rig;
    scd4x_rig_init(&rig, SCD4X_MODE_SINGLE_SHOT);
    run(&rig.source.port, &rig.clock, "scd4x_source(single_shot)+scd41_model");

    HAC_CHECK_EQ_INT((int)rig.sensor.stats.rejected, 0);
    HAC_CHECK_EQ_INT((int)rig.sensor.stats.nacked_while_busy, 0);
}

static void bme68x_source_satisfies_the_sensor_source_contract(void)
{
    static adp_clock_virtual_t clock;
    static adp_i2c_fake_t bus;
    static bme680_model_t sensor;
    static bme68x_source_t source;
    adp_clock_virtual_init(&clock, 1.0);
    adp_i2c_fake_init(&bus, &clock);
    bme680_model_init(&sensor, clock.now_ms);
    HAC_CHECK(adp_i2c_fake_attach(&bus, BME680_ADDR, bme680_model_port(&sensor)));

    const bme68x_source_config_t cfg = {
        .bus = adp_i2c_fake_port(&bus),
        .clock = adp_clock_virtual_port(&clock),
        .log = NULL,
        .address = BME680_ADDR,
        .id = BME680_SOURCE_ID,
    };
    HAC_CHECK_EQ_INT(bme68x_source_setup(&source, &cfg), HAL_OK);
    run(&source.port, &clock, "bme68x_source+bme680_model");

    // Over the real register protocol: nothing read early, nothing written
    // that the sensor dropped, nothing refused.
    HAC_CHECK_EQ_INT((int)sensor.stats.completed, 1);
    HAC_CHECK_EQ_INT((int)sensor.stats.early_reads, 0);
    HAC_CHECK_EQ_INT((int)sensor.stats.ignored_writes, 0);
    HAC_CHECK_EQ_INT((int)sensor.stats.read_only_writes, 0);
    HAC_CHECK_EQ_INT((int)sensor.stats.address_nacks, 0);
}

// --- the suite has teeth ------------------------------------------------
//
// Each case wraps a working source in one that breaks exactly one rule of
// ports/port_sensor.h, and asserts that the check meant for it -- and only
// that check -- fails.

typedef enum {
    MUTANT_REPORTS_OUTSIDE_CAPS,  // fills a field it has no capability for
    MUTANT_READS_BEFORE_START,    // returns data before start()
} mutant_kind_t;

typedef struct {
    sensor_source_t port;
    sensor_source_t *inner;
    mutant_kind_t kind;
} mutant_t;

static hal_status_t mutant_init(sensor_source_t *self)
{
    mutant_t *m = (mutant_t *)self->impl;
    return m->inner->init(m->inner);
}

static hal_status_t mutant_start(sensor_source_t *self)
{
    mutant_t *m = (mutant_t *)self->impl;
    return m->inner->start(m->inner);
}

static uint32_t mutant_ready_in_ms(const sensor_source_t *self)
{
    const mutant_t *m = (const mutant_t *)self->impl;
    return m->inner->ready_in_ms(m->inner);
}

static hal_status_t mutant_read(sensor_source_t *self, sensor_sample_t *out)
{
    mutant_t *m = (mutant_t *)self->impl;
    hal_status_t status = m->inner->read(m->inner, out);
    if (m->kind == MUTANT_READS_BEFORE_START && status == HAL_ERR_NOT_READY) {
        out->co2_ppm = measurement_ok(400.0f, 0u, self->id);
        status = HAL_OK;
    }
    if (m->kind == MUTANT_REPORTS_OUTSIDE_CAPS && status == HAL_OK) {
        out->pressure_hpa = measurement_ok(1013.0f, 0u, self->id);
    }
    return status;
}

static hal_status_t mutant_suspend(sensor_source_t *self)
{
    mutant_t *m = (mutant_t *)self->impl;
    return m->inner->suspend(m->inner);
}

static void mutant_deinit(sensor_source_t *self)
{
    mutant_t *m = (mutant_t *)self->impl;
    m->inner->deinit(m->inner);
}

#define MAX_RECORDED_FAILURES 8u

typedef struct {
    const char *failed[MAX_RECORDED_FAILURES];
    unsigned count;
} failure_log_t;

static void record_failures(void *ctx, const char *check, bool ok)
{
    failure_log_t *log = (failure_log_t *)ctx;
    if (!ok && log->count < MAX_RECORDED_FAILURES) {
        log->failed[log->count++] = check;
    }
}

static bool failed(const failure_log_t *log, const char *check)
{
    for (unsigned i = 0; i < log->count; i++) {
        if (strcmp(log->failed[i], check) == 0) {
            return true;
        }
    }
    return false;
}

static failure_log_t run_against_mutant(mutant_kind_t kind)
{
    static scd4x_rig_t rig;
    scd4x_rig_init(&rig, SCD4X_MODE_PERIODIC);

    mutant_t m = {
        .port =
            {
                .name = "mutant",
                .id = rig.source.port.id,
                .caps = rig.source.port.caps,
                .init = mutant_init,
                .start = mutant_start,
                .ready_in_ms = mutant_ready_in_ms,
                .read = mutant_read,
                .suspend = mutant_suspend,
                .deinit = mutant_deinit,
            },
        .inner = &rig.source.port,
        .kind = kind,
    };
    m.port.impl = &m;

    const port_sensor_contract_env_t env = {.advance = advance_virtual_clock, .ctx = &rig.clock};
    failure_log_t log = {.count = 0u};
    (void)port_sensor_contract_run(&m.port, &env, record_failures, &log);
    return log;
}

static void the_contract_catches_a_source_reporting_outside_its_capabilities(void)
{
    const failure_log_t log = run_against_mutant(MUTANT_REPORTS_OUTSIDE_CAPS);
    HAC_CHECK(failed(&log, "sensor.fields_outside_caps_unavailable"));
    HAC_CHECK_EQ_INT((int)log.count, 1);
}

static void the_contract_catches_a_source_reading_before_start(void)
{
    const failure_log_t log = run_against_mutant(MUTANT_READS_BEFORE_START);
    HAC_CHECK(failed(&log, "sensor.read_before_start_not_ready"));
    HAC_CHECK_EQ_INT((int)log.count, 1);
}

int main(void)
{
    HAC_RUN(replay_source_satisfies_the_sensor_source_contract);
    HAC_RUN(a_co2_only_replay_source_reports_nothing_outside_its_capability);
    HAC_RUN(scd4x_source_satisfies_the_sensor_source_contract_in_periodic_mode);
    HAC_RUN(scd4x_source_satisfies_the_sensor_source_contract_in_single_shot_mode);
    HAC_RUN(bme68x_source_satisfies_the_sensor_source_contract);
    HAC_RUN(the_contract_catches_a_source_reporting_outside_its_capabilities);
    HAC_RUN(the_contract_catches_a_source_reading_before_start);
    return hac_test_summary();
}
