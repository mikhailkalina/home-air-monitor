// SPDX-License-Identifier: Apache-2.0
//
// The round trip of docs/adr/0024: scenarios replayed through the sensor
// manager with the recorder writing what the sources report -- as the
// simulator's --record does -- and then the recordings replayed through the
// same manager as the sources that wrote them -- as --recording does -- must
// give the same merged readings, poll for poll: the same statuses, the same
// accuracies, the same source ids, and values within the decimals the format
// writes. The scenarios exercise what the format added: a warming field
// beside valid ones, accuracy rising, a sensor erroring, a stale stretch.

#ifndef HAC_TEST_TMP_DIR
#error "HAC_TEST_TMP_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_files_dir.h"
#include "app/sensor_manager.h"
#include "hac_test.h"
#include "recording/record_format.h"
#include "recording/recorder.h"
#include "replay/replay_source.h"

#define ROOT HAC_TEST_TMP_DIR "/round_trip"

#define CO2_ID 1u
#define ENV_ID 2u
#define CO2_CAPS ((sensor_caps_t)(SENS_CAP_CO2 | SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY))
#define ENV_CAPS                                                                                   \
    ((sensor_caps_t)(SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY | SENS_CAP_PRESSURE | SENS_CAP_IAQ | \
                     SENS_CAP_GAS_RES))

#define NOON_UTC 1792152000000ull  // 2026-10-16T12:00:00Z
#define RUN_MS (20u * 60u * 1000u)
#define MAX_POLLS 2000u

// The SCD41's stand-in: a warm-up, readings, a bus fault and an implausible
// value (VAL_ERROR both), recovery.
static const char CO2_SCENARIO[] = "t_offset_s,co2_ppm,temperature_c,humidity_rh,status\n"
                                   "0,,,,warmup\n"
                                   "30,650,24.1,46.0,ok\n"
                                   "120,900,24.3,47.5,ok\n"
                                   "210,,,,error\n"
                                   "270,1250,24.6,48.1,ok\n"
                                   "420,90000,24.7,48.0,out_of_range\n"
                                   "480,750,24.4,47.2,ok\n"
                                   "1500,700,24.0,46.0,ok\n";

// The BME68x as BSEC drives it: the gas and IAQ warming up beside valid
// temperature, humidity and pressure, IAQ's accuracy rising, a stale stretch.
static const char ENV_SCENARIO[] =
    "t_offset_s,temperature_c,humidity_rh,pressure_hpa,iaq,gas_res_ohm,status,iaq_status,"
    "gas_res_ohm_status,iaq_acc\n"
    "0,23.41,48.20,1006.214,,,ok,warmup,warmup,unreliable\n"
    "60,23.45,48.10,1006.200,,152000,ok,warmup,,unreliable\n"
    "197,23.50,48.00,1006.190,50.0,153000,ok,,,low\n"
    "600,23.80,47.50,1006.100,61.5,149000,ok,,,medium\n"
    "700,23.80,47.50,1006.100,61.5,149000,stale,,,medium\n"
    "760,23.90,47.40,1006.050,58.0,150500,ok,,,high\n"
    "1100,-1.25,47.00,1005.900,50.0,151000,ok,,,high\n"
    "1500,-2.50,46.00,1005.800,48.0,151500,ok,,,high\n";

typedef struct {
    uint64_t t_ms;
    air_reading_t reading;
} poll_t;

static poll_t s_first[MAX_POLLS];
static poll_t s_second[MAX_POLLS];

static bool write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    fwrite(text, 1u, strlen(text), f);
    fclose(f);
    return true;
}

static void set_priorities(sensor_manager_t *m)
{
    static const uint8_t prefer_env[] = {ENV_ID, CO2_ID};
    static const uint8_t only_env[] = {ENV_ID};
    static const uint8_t only_co2[] = {CO2_ID};
    (void)sensor_manager_set_priority(m, SENSOR_FIELD_TEMPERATURE, prefer_env, 2u);
    (void)sensor_manager_set_priority(m, SENSOR_FIELD_HUMIDITY, prefer_env, 2u);
    (void)sensor_manager_set_priority(m, SENSOR_FIELD_PRESSURE, only_env, 1u);
    (void)sensor_manager_set_priority(m, SENSOR_FIELD_CO2, only_co2, 1u);
    (void)sensor_manager_set_priority(m, SENSOR_FIELD_GAS_RESISTANCE, only_env, 1u);
    (void)sensor_manager_set_priority(m, SENSOR_FIELD_IAQ, only_env, 1u);
}

// Plays the two files through the manager for RUN_MS, the recorder writing
// to `record` when it is not NULL; returns how many polls it kept.
static size_t run(const char *co2_path, sensor_caps_t co2_caps, const char *env_path,
                  sensor_caps_t env_caps, recorder_t *record, poll_t *out)
{
    static adp_clock_virtual_t clock;
    adp_clock_virtual_init(&clock, 1.0);
    adp_clock_virtual_set_wall(&clock, NOON_UTC);
    const port_clock_t *port = adp_clock_virtual_port(&clock);

    static replay_source_t co2;
    static replay_source_t env;
    HAC_CHECK_EQ_INT(replay_source_init(&co2, co2_path, co2_caps, CO2_ID, port, true), HAL_OK);
    HAC_CHECK_EQ_INT(replay_source_init(&env, env_path, env_caps, ENV_ID, port, true), HAL_OK);
    if (co2_caps == 0) {  // a recording claims what its header shows
        co2.port.caps = replay_timeline_caps(&co2.timeline);
        env.port.caps = replay_timeline_caps(&env.timeline);
    }

    static sensor_manager_t m;
    const sensor_manager_config_t cfg = sensor_manager_config_default();
    sensor_manager_init(&m, port, NULL, &cfg);
    (void)sensor_manager_add(&m, &co2.port, SENSOR_MANAGER_DEFAULT_INTERVAL_MS);
    (void)sensor_manager_add(&m, &env.port, SENSOR_MANAGER_DEFAULT_INTERVAL_MS);
    set_priorities(&m);

    recorder_watch_t watch;
    recorder_watch_init(&watch);
    air_reading_t reading;
    air_reading_init(&reading);

    size_t n = 0u;
    while (clock.now_ms < RUN_MS && n < MAX_POLLS) {
        const uint64_t due = sensor_manager_poll(&m, &reading);
        const uint64_t now = port->now_ms(port);
        reading.wall_time_ms = port->wall_ms(port);
        air_reading_apply_age(&reading, now, cfg.max_age_ms);
        if (record != NULL) {
            recorder_event_t events[SENSOR_MANAGER_MAX_SOURCES];
            const size_t k = recorder_watch(&watch, &m, now, reading.wall_time_ms, events,
                                            SENSOR_MANAGER_MAX_SOURCES);
            for (size_t i = 0u; i < k; ++i) {
                recorder_record(record, &events[i]);
            }
            recorder_tick(record, now);
        }
        out[n].t_ms = now;
        out[n].reading = reading;
        n++;
        adp_clock_virtual_advance(&clock, (due > now) ? due - now : 1u);
    }
    replay_source_deinit(&co2);
    replay_source_deinit(&env);
    return n;
}

static const measurement_f32_t *field_of(const air_reading_t *r, sensor_field_t f)
{
    switch (f) {
        case SENSOR_FIELD_TEMPERATURE:
            return &r->temperature_c;
        case SENSOR_FIELD_HUMIDITY:
            return &r->humidity_rh;
        case SENSOR_FIELD_PRESSURE:
            return &r->pressure_hpa;
        case SENSOR_FIELD_CO2:
            return &r->co2_ppm;
        case SENSOR_FIELD_IAQ:
            return &r->iaq;
        case SENSOR_FIELD_VOC_INDEX:
            return &r->voc_index;
        case SENSOR_FIELD_GAS_RESISTANCE:
        default:
            return &r->gas_resistance_ohm;
    }
}

// Both runs read through the same parser, so a loss the parser makes in both
// would compare equal: what the first run must have carried is checked
// outright.
static void the_scenarios_carry_what_the_format_added(const poll_t *polls, size_t n)
{
    bool high_iaq = false;
    bool unreliable_warmup = false;
    bool gas_warming_beside_pressure = false;
    bool co2_error = false;
    bool below_zero = false;
    for (size_t i = 0u; i < n; ++i) {
        const air_reading_t *r = &polls[i].reading;
        high_iaq = high_iaq || (r->iaq.status == VAL_OK && r->iaq.accuracy == ACC_HIGH);
        unreliable_warmup =
            unreliable_warmup || (r->iaq.status == VAL_WARMUP && r->iaq.accuracy == ACC_UNRELIABLE);
        gas_warming_beside_pressure =
            gas_warming_beside_pressure ||
            (r->gas_resistance_ohm.status == VAL_WARMUP && r->pressure_hpa.status == VAL_OK);
        co2_error = co2_error || r->co2_ppm.status == VAL_ERROR;
        below_zero =
            below_zero || (r->temperature_c.status == VAL_OK && r->temperature_c.value < 0.0f);
    }
    HAC_CHECK(high_iaq);
    HAC_CHECK(unreliable_warmup);
    HAC_CHECK(gas_warming_beside_pressure);
    HAC_CHECK(co2_error);
    HAC_CHECK(below_zero);
}

static void a_recording_replays_the_readings_it_recorded(void)
{
    char co2_path[512];
    char env_path[512];
    snprintf(co2_path, sizeof(co2_path), "%s/round_trip_co2.csv", HAC_TEST_TMP_DIR);
    snprintf(env_path, sizeof(env_path), "%s/round_trip_env.csv", HAC_TEST_TMP_DIR);
    HAC_CHECK(write_text(co2_path, CO2_SCENARIO));
    HAC_CHECK(write_text(env_path, ENV_SCENARIO));

    // --record: into a fresh card.
    static adp_files_dir_t dir;
    HAC_CHECK_EQ_INT(adp_files_dir_init(&dir, ROOT), HAL_OK);
    port_files_t *files = adp_files_dir_port(&dir);
    (void)files->remove(files, "data/2026-10-16_scd4x.csv");
    (void)files->remove(files, "data/2026-10-16_bme68x.csv");
    static recorder_t rec;
    const time_zone_t zone = time_zone_eu(120);
    const recorder_config_t rcfg = recorder_config_default();
    recorder_init(&rec, files, NULL, &zone, &rcfg);
    HAC_CHECK_EQ_INT(recorder_add_source(&rec, CO2_ID, "scd4x", 0), HAL_OK);
    HAC_CHECK_EQ_INT(recorder_add_source(&rec, ENV_ID, "bme68x", SENS_CAP_IAQ), HAL_OK);

    const size_t first = run(co2_path, CO2_CAPS, env_path, ENV_CAPS, &rec, s_first);
    recorder_close(&rec, RUN_MS);
    adp_files_dir_deinit(&dir);
    HAC_CHECK_EQ_U64(recorder_rows_dropped(&rec), 0u);
    HAC_CHECK(recorder_rows_written(&rec) > 400u);
    the_scenarios_carry_what_the_format_added(s_first, first);

    // --recording: the files as the board names them.
    snprintf(co2_path, sizeof(co2_path), "%s/data/2026-10-16_scd4x.csv", ROOT);
    snprintf(env_path, sizeof(env_path), "%s/data/2026-10-16_bme68x.csv", ROOT);
    const size_t second =
        run(co2_path, (sensor_caps_t)0, env_path, (sensor_caps_t)0, NULL, s_second);
    HAC_CHECK_EQ_U64(second, first);

    // Poll for poll, up to the recording's last row, where its playback loops.
    unsigned compared = 0u;
    unsigned values = 0u;
    for (size_t i = 0u; i < first && i < second; ++i) {
        HAC_CHECK_EQ_U64(s_second[i].t_ms, s_first[i].t_ms);
        if (s_first[i].t_ms + SENSOR_MANAGER_DEFAULT_INTERVAL_MS >= RUN_MS) {
            break;
        }
        for (unsigned f = 0u; f < SENSOR_FIELD_COUNT; ++f) {
            const measurement_f32_t *a = field_of(&s_first[i].reading, (sensor_field_t)f);
            const measurement_f32_t *b = field_of(&s_second[i].reading, (sensor_field_t)f);
            const bool same = a->status == b->status && a->accuracy == b->accuracy &&
                              a->source_id == b->source_id;
            if (!same) {
                printf("  t=%llu ms field %u: status %d/%d accuracy %d/%d source %u/%u\n",
                       (unsigned long long)s_first[i].t_ms, f, (int)a->status, (int)b->status,
                       (int)a->accuracy, (int)b->accuracy, (unsigned)a->source_id,
                       (unsigned)b->source_id);
            }
            HAC_CHECK(same);
            if (a->status == VAL_OK) {
                const float half =
                    0.5f / powf(10.0f, (float)record_decimals((sensor_field_t)f)) + 1e-3f;
                HAC_CHECK_EQ_F32(b->value, a->value, half * (1.0f + fabsf(a->value) * 1e-6f));
                values++;
            }
        }
        compared++;
    }
    printf("  compared %u polls, %u values\n", compared, values);
    // Both sources are read at the same instants, every 5 s: one poll each,
    // all of them but the last.
    HAC_CHECK_EQ_U64(compared, RUN_MS / SENSOR_MANAGER_DEFAULT_INTERVAL_MS - 1u);
}

int main(void)
{
    HAC_RUN(a_recording_replays_the_readings_it_recorded);
    return hac_test_summary();
}
