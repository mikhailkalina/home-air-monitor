// SPDX-License-Identifier: Apache-2.0
//
// The simulator's two sensor modes on one scenario (docs/architecture.md
// 5.4): replay sources, and the real drivers -- Sensirion's and Bosch's code
// behind scd4x_source and bme68x_source -- over the fake bus, whose SCD41 and
// BME680 models measure a scenario_world played from the same CSV files. Each
// mode runs under its own sensor_manager with the board's priorities; once
// the drivers' own warm-ups are over the two readings must agree, to the
// sensors' resolution and the few seconds by which their sampling differs.
//
// It doubles as the manager's integration test over real drivers: the
// BME68x's pressure has to reach the SCD41 model through set_ambient(), and a
// failing scenario row has to become a failed read and a VAL_ERROR field.

#ifndef HAC_TEST_TMP_DIR
#error "HAC_TEST_TMP_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif

#include "scenario_world.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_i2c_fake.h"
#include "app/sensor_manager.h"
#include "bme68x_limits.h"
#include "bme68x_source.h"
#include "hac_test.h"
#include "replay/replay_source.h"
#include "scd4x_source.h"

// The sensors' own addresses (SCD4x datasheet Table 8; BME680 datasheet
// 6.2, SDO high) -- the fake bus knows nothing about the board.
#define SCD41_ADDR 0x62u
#define BME680_ADDR 0x77u

#define CO2_ID 1u
#define ENV_ID 2u
#define INTERVAL_MS 5000u

// Past the SCD4x's warm-up estimate and the BME68x's gas burn-in estimate,
// with a minute to spare; and short of the environment file's end at
// 1200 s, where playback jumps back to its first row -- one mode may have
// read the last moment before the jump and the other the first after it.
#define SETTLED_MS 360000u
#define COMPARE_UNTIL_MS 1190000u
#define STEP_MS 5000u

// How far the two modes may disagree. Their reads fall up to one interval
// apart, against ramps of 0.5 ppm/s, 1/600 C/s and so on; on top of that
// each sensor's resolution: the SCD41's CO2 in whole ppm, the BME680's gas
// ADC in 10 bits of its range.
#define CO2_TOL_PPM 5.0f
#define T_TOL_C 0.05f
#define RH_TOL 0.3f
#define P_TOL_HPA 0.1f
#define GAS_TOL_FRACTION 0.02f

static const char CO2_CSV[] = "t_offset_s,co2_ppm,temperature_c,humidity_rh,status\n"
                              "0,600,23.0,45.0,ok\n"
                              "600,900,24.0,47.0,ok\n"
                              "1200,600,23.0,45.0,ok\n";

static const char ENV_CSV[] =
    "t_offset_s,temperature_c,humidity_rh,pressure_hpa,gas_res_ohm,status\n"
    "0,21.0,50.0,1000.0,150000,ok\n"
    "1200,22.0,48.0,990.0,120000,ok\n";

static char *write_temp_csv(const char *name, const char *content)
{
    static char paths[2][512];
    static unsigned next;
    char *path = paths[next++ % 2u];
    snprintf(path, sizeof(paths[0]), "%s/%s", HAC_TEST_TMP_DIR, name);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return NULL;
    }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return path;
}

static const uint8_t PREFER_ENV[] = {ENV_ID, CO2_ID};
static const uint8_t ONLY_ENV[] = {ENV_ID};
static const uint8_t ONLY_CO2[] = {CO2_ID};

static void set_board_priorities(sensor_manager_t *m)
{
    (void)sensor_manager_set_priority(m, SENSOR_FIELD_TEMPERATURE, PREFER_ENV, 2u);
    (void)sensor_manager_set_priority(m, SENSOR_FIELD_HUMIDITY, PREFER_ENV, 2u);
    (void)sensor_manager_set_priority(m, SENSOR_FIELD_PRESSURE, ONLY_ENV, 1u);
    (void)sensor_manager_set_priority(m, SENSOR_FIELD_CO2, ONLY_CO2, 1u);
    (void)sensor_manager_set_priority(m, SENSOR_FIELD_GAS_RESISTANCE, ONLY_ENV, 1u);
}

// Polls at every deadline until the clock reaches `end_ms`, then at `end_ms`.
static void run_until(sensor_manager_t *m, adp_clock_virtual_t *clock, air_reading_t *reading,
                      uint64_t end_ms)
{
    uint64_t deadline = sensor_manager_poll(m, reading);
    while (deadline <= end_ms) {
        if (deadline > clock->now_ms) {
            adp_clock_virtual_advance(clock, deadline - clock->now_ms);
        } else {
            adp_clock_virtual_advance(clock, 1u);
        }
        deadline = sensor_manager_poll(m, reading);
    }
    if (clock->now_ms < end_ms) {
        adp_clock_virtual_advance(clock, end_ms - clock->now_ms);
        (void)sensor_manager_poll(m, reading);
    }
}

// --- replay mode ---------------------------------------------------------------

typedef struct {
    adp_clock_virtual_t clock;
    replay_source_t co2;
    replay_source_t env;
    sensor_manager_t mgr;
    air_reading_t reading;
} replay_rig_t;

static void replay_rig_init(replay_rig_t *r, const char *co2_path, const char *env_path)
{
    memset(r, 0, sizeof(*r));
    adp_clock_virtual_init(&r->clock, 1.0);
    const port_clock_t *clock = adp_clock_virtual_port(&r->clock);
    HAC_CHECK_EQ_INT(replay_source_init(&r->co2, co2_path,
                                        SENS_CAP_CO2 | SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY,
                                        CO2_ID, clock, true),
                     HAL_OK);
    HAC_CHECK_EQ_INT(replay_source_init(&r->env, env_path,
                                        SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY |
                                            SENS_CAP_PRESSURE | SENS_CAP_GAS_RES,
                                        ENV_ID, clock, true),
                     HAL_OK);

    const sensor_manager_config_t cfg = sensor_manager_config_default();
    sensor_manager_init(&r->mgr, clock, NULL, &cfg);
    HAC_CHECK_EQ_INT(sensor_manager_add(&r->mgr, &r->co2.port, INTERVAL_MS), HAL_OK);
    HAC_CHECK_EQ_INT(sensor_manager_add(&r->mgr, &r->env.port, INTERVAL_MS), HAL_OK);
    set_board_priorities(&r->mgr);
    air_reading_init(&r->reading);
}

static void replay_rig_dispose(replay_rig_t *r)
{
    replay_source_deinit(&r->co2);
    replay_source_deinit(&r->env);
}

// --- fake-i2c mode -------------------------------------------------------------

typedef struct {
    adp_clock_virtual_t clock;
    adp_i2c_fake_t bus;
    scd41_model_t scd41;
    bme680_model_t bme680;
    scenario_world_t world;
    scd4x_source_t scd4x;
    bme68x_source_t bme68x;
    sensor_manager_t mgr;
    air_reading_t reading;
} fake_rig_t;

static void fake_rig_init(fake_rig_t *r, const char *co2_path, const char *env_path)
{
    memset(r, 0, sizeof(*r));
    adp_clock_virtual_init(&r->clock, 1.0);
    const port_clock_t *clock = adp_clock_virtual_port(&r->clock);

    adp_i2c_fake_init(&r->bus, &r->clock);
    scd41_model_init(&r->scd41, r->clock.now_ms);
    bme680_model_init(&r->bme680, r->clock.now_ms);
    HAC_CHECK(adp_i2c_fake_attach(&r->bus, SCD41_ADDR, scd41_model_port(&r->scd41)));
    HAC_CHECK(adp_i2c_fake_attach(&r->bus, BME680_ADDR, bme680_model_port(&r->bme680)));

    HAC_CHECK_EQ_INT(
        scenario_world_init(&r->world, co2_path, env_path, true, HAC_BME68X_SELF_HEATING_C),
        HAL_OK);
    scenario_world_attach(&r->world, &r->scd41, &r->bme680);

    const scd4x_source_config_t scd_cfg = {
        .bus = adp_i2c_fake_port(&r->bus),
        .clock = clock,
        .log = NULL,
        .address = SCD41_ADDR,
        .id = CO2_ID,
        .mode = SCD4X_MODE_PERIODIC,
    };
    HAC_CHECK_EQ_INT(scd4x_source_setup(&r->scd4x, &scd_cfg), HAL_OK);
    const bme68x_source_config_t bme_cfg = {
        .bus = adp_i2c_fake_port(&r->bus),
        .clock = clock,
        .log = NULL,
        .address = BME680_ADDR,
        .id = ENV_ID,
    };
    HAC_CHECK_EQ_INT(bme68x_source_setup(&r->bme68x, &bme_cfg), HAL_OK);

    const sensor_manager_config_t cfg = sensor_manager_config_default();
    sensor_manager_init(&r->mgr, clock, NULL, &cfg);
    HAC_CHECK_EQ_INT(sensor_manager_add(&r->mgr, &r->scd4x.port, INTERVAL_MS), HAL_OK);
    HAC_CHECK_EQ_INT(sensor_manager_add(&r->mgr, &r->bme68x.port, INTERVAL_MS), HAL_OK);
    set_board_priorities(&r->mgr);
    air_reading_init(&r->reading);
}

static void fake_rig_dispose(fake_rig_t *r)
{
    r->scd4x.port.deinit(&r->scd4x.port);  // one SCD4x per image: release the HAL
    r->bme68x.port.deinit(&r->bme68x.port);
    scenario_world_dispose(&r->world);
}

// --- the cases -------------------------------------------------------------------

static void close_to(const measurement_f32_t *a, const measurement_f32_t *b, float tol,
                     const char *what, uint64_t at_ms)
{
    const bool same_status = a->status == VAL_OK && b->status == VAL_OK;
    const bool close = fabsf(a->value - b->value) <= tol;
    if (!same_status || !close) {
        printf("  %s at %llu ms: replay %.3f (status %d), fake-i2c %.3f (status %d)\n", what,
               (unsigned long long)at_ms, (double)a->value, (int)a->status, (double)b->value,
               (int)b->status);
    }
    HAC_CHECK(same_status);
    HAC_CHECK(close);
}

static void both_modes_show_the_same_room_once_the_sensors_have_settled(void)
{
    char *co2_path = write_temp_csv("world_co2.csv", CO2_CSV);
    char *env_path = write_temp_csv("world_env.csv", ENV_CSV);
    HAC_CHECK(co2_path != NULL && env_path != NULL);

    static replay_rig_t replay;
    static fake_rig_t fake;
    replay_rig_init(&replay, co2_path, env_path);
    fake_rig_init(&fake, co2_path, env_path);

    for (uint64_t t = SETTLED_MS; t <= COMPARE_UNTIL_MS; t += STEP_MS) {
        run_until(&replay.mgr, &replay.clock, &replay.reading, t);
        run_until(&fake.mgr, &fake.clock, &fake.reading, t);

        const air_reading_t *a = &replay.reading;
        const air_reading_t *b = &fake.reading;
        close_to(&a->co2_ppm, &b->co2_ppm, CO2_TOL_PPM, "co2", t);
        close_to(&a->temperature_c, &b->temperature_c, T_TOL_C, "temperature", t);
        close_to(&a->humidity_rh, &b->humidity_rh, RH_TOL, "humidity", t);
        close_to(&a->pressure_hpa, &b->pressure_hpa, P_TOL_HPA, "pressure", t);
        close_to(&a->gas_resistance_ohm, &b->gas_resistance_ohm,
                 GAS_TOL_FRACTION * a->gas_resistance_ohm.value, "gas", t);
        HAC_CHECK_EQ_INT(b->temperature_c.source_id, ENV_ID);  // the BME68x, not the fallback
        HAC_CHECK_EQ_INT(b->co2_ppm.source_id, CO2_ID);
    }

    // The BME68x's pressure reached the SCD41 through set_ambient(), in
    // whole hectopascals (its datasheet, 3.7.5).
    const float pressure = fake.reading.pressure_hpa.value;
    HAC_CHECK(fabsf((float)fake.scd41.ambient_pressure_hpa - pressure) <= 1.0f);
    HAC_CHECK(fake.scd41.ambient_pressure_hpa != 1013u);  // not the sensor's default

    // Nothing on the bus was refused along the way.
    HAC_CHECK_EQ_INT((int)fake.scd41.stats.rejected, 0);
    HAC_CHECK_EQ_INT((int)fake.bme680.stats.early_reads, 0);
    HAC_CHECK_EQ_INT((int)fake.bme680.stats.ignored_writes, 0);
    HAC_CHECK_EQ_INT((int)sensor_manager_slot(&fake.mgr, 0)->failures, 0);
    HAC_CHECK_EQ_INT((int)sensor_manager_slot(&fake.mgr, 1)->failures, 0);

    replay_rig_dispose(&replay);
    fake_rig_dispose(&fake);
}

static void the_drivers_warm_up_where_the_replay_does_not(void)
{
    char *co2_path = write_temp_csv("world_co2.csv", CO2_CSV);
    char *env_path = write_temp_csv("world_env.csv", ENV_CSV);
    static fake_rig_t fake;
    fake_rig_init(&fake, co2_path, env_path);

    run_until(&fake.mgr, &fake.clock, &fake.reading, 30000u);
    HAC_CHECK_EQ_INT(fake.reading.co2_ppm.status, VAL_WARMUP);             // SCD4X_WARMUP_MS
    HAC_CHECK_EQ_INT(fake.reading.gas_resistance_ohm.status, VAL_WARMUP);  // burn-in
    HAC_CHECK_EQ_INT(fake.reading.temperature_c.status, VAL_OK);           // from the BME68x
    HAC_CHECK_EQ_INT(fake.reading.pressure_hpa.status, VAL_OK);
    fake_rig_dispose(&fake);
}

static void a_failing_row_is_a_failed_read_on_the_bus(void)
{
    // CO2 fails from 400 s to 460 s; the rest of the room carries on.
    char *co2_path = write_temp_csv("world_co2_fault.csv",
                                    "t_offset_s,co2_ppm,temperature_c,humidity_rh,status\n"
                                    "0,600,23.0,45.0,ok\n"
                                    "400,0,0,0,error\n"
                                    "460,600,23.0,45.0,ok\n"
                                    "3600,600,23.0,45.0,ok\n");
    char *env_path = write_temp_csv("world_env.csv", ENV_CSV);
    static fake_rig_t fake;
    fake_rig_init(&fake, co2_path, env_path);

    run_until(&fake.mgr, &fake.clock, &fake.reading, 430000u);
    HAC_CHECK_EQ_INT(fake.reading.co2_ppm.status, VAL_ERROR);
    HAC_CHECK(fake.world.scd41_failed_rows > 0u);
    HAC_CHECK_EQ_INT(fake.reading.temperature_c.status, VAL_OK);
    HAC_CHECK_EQ_INT(fake.reading.pressure_hpa.status, VAL_OK);

    // Back once the rows are ok: started over by the manager, warmed up by
    // the driver, and reading.
    run_until(&fake.mgr, &fake.clock, &fake.reading, 600000u);
    HAC_CHECK_EQ_INT(fake.reading.co2_ppm.status, VAL_OK);
    HAC_CHECK_EQ_F32(fake.reading.co2_ppm.value, 600.0f, 1.0f);
    HAC_CHECK(sensor_manager_slot(&fake.mgr, 0)->inits >= 2u);
    fake_rig_dispose(&fake);
}

int main(void)
{
    HAC_RUN(both_modes_show_the_same_room_once_the_sensors_have_settled);
    HAC_RUN(the_drivers_warm_up_where_the_replay_does_not);
    HAC_RUN(a_failing_row_is_a_failed_read_on_the_bus);
    return hac_test_summary();
}
