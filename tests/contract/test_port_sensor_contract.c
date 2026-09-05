// SPDX-License-Identifier: Apache-2.0
//
// Contract that every sensor_source_t implementation must satisfy (see
// CLAUDE.md: "every new port needs a suite in tests/contract/ that runs
// against all of its implementations"). replay_source is the only
// implementation so far; a future fake-i2c-backed driver would add another
// call to run_sensor_source_contract() rather than a new suite.

#ifndef HAC_TEST_TMP_DIR
#error "HAC_TEST_TMP_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif

#include <stdio.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "hac_test.h"
#include "replay/replay_source.h"

// Zeros every measurement in `s` whose capability bit is not set in `caps`,
// then compares against a freshly zeroed sample: a source must never report
// data for a quantity it did not declare.
static bool fields_outside_caps_are_unavailable(const sensor_sample_t *s, sensor_caps_t caps)
{
    if (!(caps & SENS_CAP_TEMPERATURE) && s->temperature_c.status != VAL_UNAVAILABLE) {
        return false;
    }
    if (!(caps & SENS_CAP_HUMIDITY) && s->humidity_rh.status != VAL_UNAVAILABLE) {
        return false;
    }
    if (!(caps & SENS_CAP_PRESSURE) && s->pressure_hpa.status != VAL_UNAVAILABLE) {
        return false;
    }
    if (!(caps & SENS_CAP_CO2) && s->co2_ppm.status != VAL_UNAVAILABLE) {
        return false;
    }
    if (!(caps & SENS_CAP_IAQ) && s->iaq.status != VAL_UNAVAILABLE) {
        return false;
    }
    if (!(caps & SENS_CAP_VOC) && s->voc_index.status != VAL_UNAVAILABLE) {
        return false;
    }
    if (!(caps & SENS_CAP_GAS_RES) && s->gas_resistance_ohm.status != VAL_UNAVAILABLE) {
        return false;
    }
    return true;
}

// `advance` moves whatever clock backs `src` forward by `ms` of virtual time;
// it decouples the contract from any one clock implementation.
typedef void (*advance_fn)(void *ctx, uint64_t ms);

static void run_sensor_source_contract(sensor_source_t *src, advance_fn advance, void *advance_ctx)
{
    HAC_CHECK(src->name != NULL);
    HAC_CHECK(src->caps != 0);

    HAC_CHECK_EQ_INT(src->init(src), HAL_OK);

    sensor_sample_t sample;
    HAC_CHECK_EQ_INT(src->read(src, &sample), HAL_ERR_NOT_READY);  // nothing before start()

    HAC_CHECK_EQ_INT(src->start(src), HAL_OK);

    // Give the source a generous number of chances to become ready, in case
    // a real implementation needs several polls (e.g. a warm-up period).
    hal_status_t status = HAL_ERR_NOT_READY;
    for (int attempt = 0; attempt < 8 && status != HAL_OK; attempt++) {
        uint32_t wait_ms = src->ready_in_ms(src);
        advance(advance_ctx, wait_ms > 0 ? wait_ms : 1u);
        status = src->read(src, &sample);
    }
    HAC_CHECK_EQ_INT(status, HAL_OK);
    HAC_CHECK(fields_outside_caps_are_unavailable(&sample, src->caps));

    HAC_CHECK_EQ_INT(src->suspend(src), HAL_OK);
    src->deinit(src);  // must not crash; the source is unusable after this
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

    run_sensor_source_contract(&rs.port, advance_virtual_clock, &vc);
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

    run_sensor_source_contract(&rs.port, advance_virtual_clock, &vc);
}

int main(void)
{
    HAC_RUN(replay_source_satisfies_the_sensor_source_contract);
    HAC_RUN(a_co2_only_replay_source_reports_nothing_outside_its_capability);
    return hac_test_summary();
}
