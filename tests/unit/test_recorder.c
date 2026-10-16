// SPDX-License-Identifier: Apache-2.0
//
// core/recording/recorder.c: the watch over the sensor manager's slots, and
// the recorder writing rows through adp_files_dir into a scratch directory
// -- the files read back with stdio, as the simulator and a spreadsheet
// would read them off a card.

#ifndef HAC_TEST_TMP_DIR
#error "HAC_TEST_TMP_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif

#define _POSIX_C_SOURCE 200809L

#include "recording/recorder.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "adp_files_dir.h"
#include "hac_test.h"
#include "recording/record_limits.h"
#include "replay/replay_source.h"

#define ROOT HAC_TEST_TMP_DIR "/recorder"

#define SCD4X_ID 1u
#define BME68X_ID 2u
#define SCD4X_CAPS ((sensor_caps_t)(SENS_CAP_CO2 | SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY))
#define BSEC_CAPS                                                                                  \
    ((sensor_caps_t)(SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY | SENS_CAP_PRESSURE | SENS_CAP_IAQ | \
                     SENS_CAP_GAS_RES))

// 2026-10-16T12:00:00Z: 15:00 in Vilnius, summer time (UTC+3).
#define NOON_UTC 1792152000000ull
// Local midnight between the 16th and the 17th: 21:00 UTC.
#define MIDNIGHT_UTC (NOON_UTC + 9ull * 3600000ull)

#define BOOT_MS 10000u

// --- a log that counts -------------------------------------------------------------

#define LOG_LINES_MAX 64u

typedef struct {
    port_log_t port;
    const char *msgs[LOG_LINES_MAX];
    unsigned count;
} counting_log_t;

static void counting_write(const port_log_t *self, log_level_t level, const char *tag,
                           const char *msg, const log_field_t *fields, size_t field_count)
{
    (void)level;
    (void)tag;
    (void)fields;
    (void)field_count;
    counting_log_t *log = (counting_log_t *)self->impl;
    if (log->count < LOG_LINES_MAX) {
        log->msgs[log->count++] = msg;
    }
}

static unsigned lines(const counting_log_t *log, const char *msg)
{
    unsigned n = 0u;
    for (unsigned i = 0u; i < log->count; ++i) {
        n += (strcmp(log->msgs[i], msg) == 0) ? 1u : 0u;
    }
    return n;
}

// --- the rig -------------------------------------------------------------------------

typedef struct {
    adp_files_dir_t dir;
    counting_log_t log;
    time_zone_t zone;
    recorder_t rec;
} rig_t;

static void wipe(const char *dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", ROOT, dir);
    DIR *d = opendir(path);
    if (d == NULL) {
        return;
    }
    for (struct dirent *e = readdir(d); e != NULL; e = readdir(d)) {
        if (e->d_name[0] != '.') {
            char file[1024];
            snprintf(file, sizeof(file), "%s/%s", path, e->d_name);
            (void)unlink(file);
        }
    }
    closedir(d);
}

static void rig_init(rig_t *r, const recorder_config_t *cfg)
{
    memset(r, 0, sizeof(*r));
    HAC_CHECK_EQ_INT(adp_files_dir_init(&r->dir, ROOT), HAL_OK);
    wipe(RECORD_DATA_DIR);
    r->log.port.write = counting_write;
    r->log.port.impl = &r->log;
    r->zone = time_zone_eu(120);
    const recorder_config_t def = recorder_config_default();
    recorder_init(&r->rec, adp_files_dir_port(&r->dir), &r->log.port, &r->zone,
                  (cfg != NULL) ? cfg : &def);
    HAC_CHECK_EQ_INT(recorder_add_source(&r->rec, SCD4X_ID, "scd4x", 0), HAL_OK);
    HAC_CHECK_EQ_INT(recorder_add_source(&r->rec, BME68X_ID, "bme68x", SENS_CAP_IAQ), HAL_OK);
}

static void rig_dispose(rig_t *r, uint64_t now_ms)
{
    recorder_close(&r->rec, now_ms);
    adp_files_dir_deinit(&r->dir);
}

static recorder_event_t scd4x_event(uint64_t now_ms, uint64_t wall_ms, float co2)
{
    recorder_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.source_id = SCD4X_ID;
    ev.caps = SCD4X_CAPS;
    ev.sample.co2_ppm = measurement_ok(co2, now_ms, SCD4X_ID);
    ev.sample.temperature_c = measurement_ok(24.5f, now_ms, SCD4X_ID);
    ev.sample.humidity_rh = measurement_ok(47.0f, now_ms, SCD4X_ID);
    ev.now_ms = now_ms;
    ev.wall_ms = wall_ms;
    return ev;
}

static size_t read_file(const char *rel, char *buf, size_t cap)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", ROOT, rel);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        buf[0] = '\0';
        return 0u;
    }
    const size_t n = fread(buf, 1u, cap - 1u, f);
    fclose(f);
    buf[n] = '\0';
    return n;
}

static bool exists(const char *rel)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", ROOT, rel);
    return access(path, F_OK) == 0;
}

static unsigned count_lines(const char *text)
{
    unsigned n = 0u;
    for (; *text != '\0'; ++text) {
        n += (*text == '\n') ? 1u : 0u;
    }
    return n;
}

// --- the watch -----------------------------------------------------------------------

static hal_status_t stub_ok(sensor_source_t *self)
{
    (void)self;
    return HAL_OK;
}

static void the_watch_reports_new_samples_and_new_failures_once(void)
{
    static sensor_source_t a = {.name = "a", .id = 1u, .caps = SCD4X_CAPS, .init = stub_ok};
    static sensor_source_t b = {.name = "b", .id = 2u, .caps = BSEC_CAPS, .init = stub_ok};
    static sensor_manager_t m;
    static const port_clock_t no_clock = {0};
    const sensor_manager_config_t cfg = sensor_manager_config_default();
    sensor_manager_init(&m, &no_clock, NULL, &cfg);
    HAC_CHECK_EQ_INT(sensor_manager_add(&m, &a, 5000u), HAL_OK);
    HAC_CHECK_EQ_INT(sensor_manager_add(&m, &b, 3000u), HAL_OK);

    recorder_watch_t w;
    recorder_watch_init(&w);
    recorder_event_t ev[4];
    HAC_CHECK_EQ_U64(recorder_watch(&w, &m, 100u, 0u, ev, 4u), 0u);

    m.slots[0].reads = 1u;
    m.slots[0].sample.co2_ppm = measurement_ok(600.0f, 90u, 1u);
    HAC_CHECK_EQ_U64(recorder_watch(&w, &m, 100u, NOON_UTC, ev, 4u), 1u);
    HAC_CHECK_EQ_INT(ev[0].source_id, 1);
    HAC_CHECK(!ev[0].failed);
    HAC_CHECK_EQ_INT((int)ev[0].caps, (int)SCD4X_CAPS);
    HAC_CHECK_EQ_F32(ev[0].sample.co2_ppm.value, 600.0f, 0.001f);
    HAC_CHECK_EQ_U64(ev[0].wall_ms, NOON_UTC);
    HAC_CHECK_EQ_U64(recorder_watch(&w, &m, 200u, 0u, ev, 4u), 0u);  // nothing new

    m.slots[1].failures = 1u;
    m.slots[0].reads = 2u;
    HAC_CHECK_EQ_U64(recorder_watch(&w, &m, 300u, 0u, ev, 4u), 2u);
    HAC_CHECK_EQ_INT(ev[1].source_id, 2);
    HAC_CHECK(ev[1].failed);

    // No room: the change waits for the next call.
    m.slots[0].reads = 3u;
    m.slots[1].reads = 1u;
    HAC_CHECK_EQ_U64(recorder_watch(&w, &m, 400u, 0u, ev, 1u), 1u);
    HAC_CHECK_EQ_INT(ev[0].source_id, 1);
    HAC_CHECK_EQ_U64(recorder_watch(&w, &m, 500u, 0u, ev, 4u), 1u);
    HAC_CHECK_EQ_INT(ev[0].source_id, 2);
}

// --- the recorder ----------------------------------------------------------------------

static void sources_are_added_with_valid_tags_only(void)
{
    static rig_t r;
    rig_init(&r, NULL);
    HAC_CHECK_EQ_INT(recorder_add_source(&r.rec, 0u, "x", 0), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(recorder_add_source(&r.rec, SCD4X_ID, "again", 0), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(recorder_add_source(&r.rec, 3u, "Bad_Tag", 0), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(recorder_add_source(&r.rec, 3u, "third", 0), HAL_OK);
    HAC_CHECK_EQ_INT(recorder_add_source(&r.rec, 4u, "fourth", 0), HAL_OK);
    HAC_CHECK_EQ_INT(recorder_add_source(&r.rec, 5u, "fifth", 0), HAL_ERR_NO_MEM);

    // A source never added is not recorded.
    recorder_event_t ev = scd4x_event(BOOT_MS, NOON_UTC, 600.0f);
    ev.source_id = 9u;
    recorder_record(&r.rec, &ev);
    HAC_CHECK_EQ_INT(r.rec.state, RECORDER_WAITING);
    rig_dispose(&r, BOOT_MS);
}

static void rows_reach_the_card_at_the_flush_interval(void)
{
    static rig_t r;
    rig_init(&r, NULL);
    char text[4096];

    recorder_event_t ev = scd4x_event(BOOT_MS, NOON_UTC, 600.0f);
    recorder_record(&r.rec, &ev);
    HAC_CHECK_EQ_INT(r.rec.state, RECORDER_ON);
    HAC_CHECK_EQ_U64(lines(&r.log, "recording"), 1u);
    HAC_CHECK(exists("data/2026-10-16_scd4x.csv"));
    HAC_CHECK_EQ_U64(read_file("data/2026-10-16_scd4x.csv", text, sizeof(text)), 0u);

    // Rows wait in RAM until the interval has passed...
    for (unsigned i = 1u; i <= 3u; ++i) {
        ev = scd4x_event(BOOT_MS + i * 2000u, NOON_UTC + i * 2000u, 600.0f + (float)i);
        // Taken a little before the event: the poll took 200 ms.
        ev.sample.co2_ppm.ts_ms = ev.now_ms - 200u;
        ev.sample.temperature_c.ts_ms = ev.now_ms - 200u;
        ev.sample.humidity_rh.ts_ms = ev.now_ms - 200u;
        recorder_record(&r.rec, &ev);
        recorder_tick(&r.rec, ev.now_ms);
    }
    HAC_CHECK_EQ_U64(read_file("data/2026-10-16_scd4x.csv", text, sizeof(text)), 0u);

    // ...and then go out together, flushed.
    recorder_tick(&r.rec, BOOT_MS + RECORD_FLUSH_INTERVAL_MS);
    read_file("data/2026-10-16_scd4x.csv", text, sizeof(text));
    HAC_CHECK(strstr(text, "# home-air-monitor recording: source scd4x, id 1\n") == text);
    HAC_CHECK(strstr(text, "t_offset_s,utc,temperature_c,humidity_rh,co2_ppm,status,"
                           "temperature_c_status,humidity_rh_status,co2_ppm_status\n") != NULL);
    HAC_CHECK(strstr(text, "\n0.000,2026-10-16T12:00:00Z,24.50,47.00,600,ok,,,\n") != NULL);
    // t_offset counts from the first row; utc is the wall time minus the
    // row's age (200 ms): 12:00:01.800 -> 12:00:01.
    HAC_CHECK(strstr(text, "\n1.800,2026-10-16T12:00:01Z,24.50,47.00,601,ok,,,\n") != NULL);
    HAC_CHECK_EQ_U64(count_lines(text), 6u);
    HAC_CHECK_EQ_U64(recorder_rows_written(&r.rec), 4u);
    HAC_CHECK_EQ_U64(recorder_rows_dropped(&r.rec), 0u);
    HAC_CHECK_EQ_U64(recorder_bytes_written(&r.rec), strlen(text));

    // What it wrote is a scenario the simulator plays.
    replay_timeline_t tl;
    HAC_CHECK_EQ_INT(replay_parse_buffer(text, strlen(text), &tl), HAL_OK);
    HAC_CHECK_EQ_U64(tl.row_count, 4u);
    HAC_CHECK_EQ_INT((int)replay_timeline_caps(&tl), (int)SCD4X_CAPS);
    HAC_CHECK_EQ_U64(tl.first_utc_ms, NOON_UTC);
    replay_timeline_dispose(&tl);
    rig_dispose(&r, BOOT_MS + RECORD_FLUSH_INTERVAL_MS);
}

static void each_source_has_its_file_and_its_accuracy_column(void)
{
    static rig_t r;
    rig_init(&r, NULL);
    char text[4096];

    recorder_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.source_id = BME68X_ID;
    ev.caps = BSEC_CAPS;
    ev.sample.temperature_c = measurement_ok(23.41f, BOOT_MS, BME68X_ID);
    ev.sample.humidity_rh = measurement_ok(48.2f, BOOT_MS, BME68X_ID);
    ev.sample.pressure_hpa = measurement_ok(1006.214f, BOOT_MS, BME68X_ID);
    ev.sample.iaq = measurement_ok(50.0f, BOOT_MS, BME68X_ID);
    ev.sample.iaq.accuracy = ACC_LOW;
    ev.sample.gas_resistance_ohm = measurement_status(VAL_WARMUP, BOOT_MS, BME68X_ID);
    ev.now_ms = BOOT_MS;
    ev.wall_ms = NOON_UTC;
    recorder_record(&r.rec, &ev);
    const recorder_event_t co2 = scd4x_event(BOOT_MS, NOON_UTC, 700.0f);
    recorder_record(&r.rec, &co2);
    recorder_flush(&r.rec, BOOT_MS);

    read_file("data/2026-10-16_bme68x.csv", text, sizeof(text));
    HAC_CHECK(strstr(text, ",iaq_acc\n0.000,2026-10-16T12:00:00Z,23.41,48.20,1006.214,50.00,,ok,,"
                           ",,,warmup,low\n") != NULL);
    read_file("data/2026-10-16_scd4x.csv", text, sizeof(text));
    HAC_CHECK(strstr(text, "_acc") == NULL);
    HAC_CHECK(strstr(text, ",700,ok,,,\n") != NULL);
    rig_dispose(&r, BOOT_MS);
}

static void a_failure_is_a_row_of_errors(void)
{
    static rig_t r;
    rig_init(&r, NULL);
    char text[4096];
    recorder_event_t ev = scd4x_event(BOOT_MS, NOON_UTC, 600.0f);
    ev.failed = true;
    ev.sample.co2_ppm = measurement_status(VAL_ERROR, BOOT_MS, SCD4X_ID);
    ev.sample.temperature_c = measurement_status(VAL_ERROR, BOOT_MS, SCD4X_ID);
    ev.sample.humidity_rh = measurement_status(VAL_ERROR, BOOT_MS, SCD4X_ID);
    recorder_record(&r.rec, &ev);
    recorder_flush(&r.rec, BOOT_MS);
    read_file("data/2026-10-16_scd4x.csv", text, sizeof(text));
    HAC_CHECK(strstr(text, "\n0.000,2026-10-16T12:00:00Z,,,,error,,,\n") != NULL);
    rig_dispose(&r, BOOT_MS);
}

// Vilnius midnight, not UTC's: the new file starts at 21:00 UTC in summer.
static void the_file_turns_at_local_midnight(void)
{
    static rig_t r;
    rig_init(&r, NULL);
    char text[4096];

    const uint64_t before = MIDNIGHT_UTC - 1000u;
    recorder_event_t ev = scd4x_event(BOOT_MS, before - 5000u, 600.0f);
    recorder_record(&r.rec, &ev);
    ev = scd4x_event(BOOT_MS + 5000u, before, 610.0f);
    recorder_record(&r.rec, &ev);
    // 21:00:04Z is 00:00:04 on the 17th in Vilnius.
    ev = scd4x_event(BOOT_MS + 10000u, MIDNIGHT_UTC + 4000u, 620.0f);
    recorder_record(&r.rec, &ev);

    // The rotation put the 16th's rows on the card at once, no tick needed.
    read_file("data/2026-10-16_scd4x.csv", text, sizeof(text));
    HAC_CHECK_EQ_U64(count_lines(text), 4u);
    HAC_CHECK(strstr(text, "\n5.000,2026-10-16T20:59:59Z,24.50,47.00,610,ok,,,\n") != NULL);

    recorder_flush(&r.rec, BOOT_MS + 10000u);
    read_file("data/2026-10-17_scd4x.csv", text, sizeof(text));
    HAC_CHECK(strstr(text, "\n0.000,2026-10-16T21:00:04Z,24.50,47.00,620,ok,,,\n") != NULL);
    HAC_CHECK_EQ_U64(r.rec.sources[0].files_opened, 2u);
    rig_dispose(&r, BOOT_MS + 10000u);
}

// Before the wall clock: an undated file, numbered past those already there;
// once the clock is set, the day's file.
static void rows_without_a_clock_go_to_an_undated_file(void)
{
    static rig_t r;
    rig_init(&r, NULL);
    char text[4096];
    port_files_t *f = adp_files_dir_port(&r.dir);
    port_file_t *old = NULL;
    HAC_CHECK_EQ_INT(f->open_append(f, "data/undated-0004_bme68x.csv", &old), HAL_OK);
    HAC_CHECK_EQ_INT(f->close(f, old), HAL_OK);

    recorder_event_t ev = scd4x_event(BOOT_MS, 0u, 600.0f);
    recorder_record(&r.rec, &ev);
    HAC_CHECK(exists("data/undated-0005_scd4x.csv"));
    ev = scd4x_event(BOOT_MS + 5000u, NOON_UTC, 610.0f);  // time_set
    recorder_record(&r.rec, &ev);
    recorder_flush(&r.rec, BOOT_MS + 5000u);

    read_file("data/undated-0005_scd4x.csv", text, sizeof(text));
    HAC_CHECK(strstr(text, "\n0.000,,24.50,47.00,600,ok,,,\n") != NULL);
    read_file("data/2026-10-16_scd4x.csv", text, sizeof(text));
    HAC_CHECK(strstr(text, "\n0.000,2026-10-16T12:00:00Z,24.50,47.00,610,ok,,,\n") != NULL);
    rig_dispose(&r, BOOT_MS + 5000u);
}

// A second boot on the same day starts the day's next file: a file is one
// monotonic clock, and the first one's is gone.
static void a_closed_file_is_never_reopened(void)
{
    static rig_t r;
    rig_init(&r, NULL);
    recorder_event_t ev = scd4x_event(BOOT_MS, NOON_UTC, 600.0f);
    recorder_record(&r.rec, &ev);
    rig_dispose(&r, BOOT_MS);

    static rig_t again;
    memset(&again, 0, sizeof(again));
    HAC_CHECK_EQ_INT(adp_files_dir_init(&again.dir, ROOT), HAL_OK);  // no wipe: the same card
    again.log.port.write = counting_write;
    again.log.port.impl = &again.log;
    again.zone = time_zone_eu(120);
    const recorder_config_t cfg = recorder_config_default();
    recorder_init(&again.rec, adp_files_dir_port(&again.dir), &again.log.port, &again.zone, &cfg);
    HAC_CHECK_EQ_INT(recorder_add_source(&again.rec, SCD4X_ID, "scd4x", 0), HAL_OK);
    ev = scd4x_event(1000u, NOON_UTC + 3600000u, 650.0f);
    recorder_record(&again.rec, &ev);
    HAC_CHECK(exists("data/2026-10-16_scd4x_2.csv"));
    rig_dispose(&again, 1000u);
}

static void a_missing_medium_is_a_state_said_once(void)
{
    static rig_t r;
    rig_init(&r, NULL);
    adp_files_dir_set_present(&r.dir, false);

    for (unsigned i = 0u; i < 10u; ++i) {
        const recorder_event_t ev = scd4x_event(BOOT_MS + i * 5000u, NOON_UTC + i * 5000u, 600.0f);
        recorder_record(&r.rec, &ev);
        recorder_tick(&r.rec, ev.now_ms);
    }
    HAC_CHECK_EQ_INT(r.rec.state, RECORDER_NO_MEDIUM);
    HAC_CHECK_EQ_U64(recorder_rows_dropped(&r.rec), 10u);
    HAC_CHECK_EQ_U64(lines(&r.log, "recording stopped"), 1u);

    // A card goes in: the next row is recorded, no retry wait.
    adp_files_dir_set_present(&r.dir, true);
    const recorder_event_t ev = scd4x_event(BOOT_MS + 50000u, NOON_UTC + 50000u, 600.0f);
    recorder_record(&r.rec, &ev);
    HAC_CHECK_EQ_INT(r.rec.state, RECORDER_ON);
    HAC_CHECK(exists("data/2026-10-16_scd4x.csv"));

    // And taken out under an open file: the next flush finds it gone.
    adp_files_dir_set_present(&r.dir, false);
    recorder_flush(&r.rec, BOOT_MS + 55000u);
    HAC_CHECK_EQ_INT(r.rec.state, RECORDER_NO_MEDIUM);
    HAC_CHECK(r.rec.sources[0].file == NULL);
    HAC_CHECK_EQ_U64(lines(&r.log, "recording stopped"), 2u);
    adp_files_dir_set_present(&r.dir, true);
    rig_dispose(&r, BOOT_MS + 55000u);
}

static void a_full_medium_stops_recording_and_is_retried(void)
{
    static rig_t r;
    rig_init(&r, NULL);
    adp_files_dir_set_limit(&r.dir, 1u);

    recorder_event_t ev = scd4x_event(BOOT_MS, NOON_UTC, 600.0f);
    recorder_record(&r.rec, &ev);
    recorder_flush(&r.rec, BOOT_MS);
    HAC_CHECK_EQ_INT(r.rec.state, RECORDER_FULL);
    HAC_CHECK_EQ_U64(recorder_rows_dropped(&r.rec), 1u);
    // The file nothing reached is not left behind.
    HAC_CHECK(!exists("data/2026-10-16_scd4x.csv"));

    // Before the retry, nothing is opened.
    ev = scd4x_event(BOOT_MS + 5000u, NOON_UTC + 5000u, 600.0f);
    recorder_record(&r.rec, &ev);
    HAC_CHECK(!exists("data/2026-10-16_scd4x.csv"));
    HAC_CHECK_EQ_U64(recorder_rows_dropped(&r.rec), 2u);

    // Room again, and the retry's time: on.
    adp_files_dir_set_limit(&r.dir, 0u);
    ev = scd4x_event(BOOT_MS + RECORD_RETRY_MS, NOON_UTC + RECORD_RETRY_MS, 600.0f);
    recorder_record(&r.rec, &ev);
    recorder_flush(&r.rec, ev.now_ms);
    HAC_CHECK_EQ_INT(r.rec.state, RECORDER_ON);
    HAC_CHECK(exists("data/2026-10-16_scd4x.csv"));
    HAC_CHECK_EQ_U64(lines(&r.log, "recording stopped"), 1u);
    rig_dispose(&r, ev.now_ms);
}

// More rows in one interval than the buffer holds: appended early, none lost.
static void a_full_buffer_is_appended_early(void)
{
    static rig_t r;
    rig_init(&r, NULL);
    char text[16384];
    for (unsigned i = 0u; i < 100u; ++i) {
        const recorder_event_t ev = scd4x_event(BOOT_MS + i * 10u, NOON_UTC + i * 10u, 600.0f);
        recorder_record(&r.rec, &ev);
    }
    HAC_CHECK(read_file("data/2026-10-16_scd4x.csv", text, sizeof(text)) > 0u);
    recorder_flush(&r.rec, BOOT_MS + 1000u);
    read_file("data/2026-10-16_scd4x.csv", text, sizeof(text));
    HAC_CHECK_EQ_U64(count_lines(text), 102u);
    HAC_CHECK_EQ_U64(recorder_rows_dropped(&r.rec), 0u);
    rig_dispose(&r, BOOT_MS + 1000u);
}

// The bound runs when a file opens: the oldest days go, today never.
static void opening_a_file_keeps_data_within_its_bound(void)
{
    recorder_config_t cfg = recorder_config_default();
    cfg.dir_cap_bytes = 100u;
    static rig_t r;
    rig_init(&r, &cfg);
    port_files_t *f = adp_files_dir_port(&r.dir);
    static const char *const OLD[] = {"data/2026-10-14_scd4x.csv", "data/2026-10-15_scd4x.csv",
                                      "data/2026-10-16_bme68x.csv"};
    for (size_t i = 0u; i < 3u; ++i) {
        port_file_t *file = NULL;
        HAC_CHECK_EQ_INT(f->open_append(f, OLD[i], &file), HAL_OK);
        HAC_CHECK_EQ_INT(
            f->append(f, file, "0123456789012345678901234567890123456789012345678", 49u), HAL_OK);
        HAC_CHECK_EQ_INT(f->close(f, file), HAL_OK);
    }

    const recorder_event_t ev = scd4x_event(BOOT_MS, NOON_UTC, 600.0f);
    recorder_record(&r.rec, &ev);
    HAC_CHECK(!exists("data/2026-10-14_scd4x.csv"));
    HAC_CHECK(exists("data/2026-10-15_scd4x.csv"));
    HAC_CHECK(exists("data/2026-10-16_bme68x.csv"));  // the day being recorded
    HAC_CHECK_EQ_U64(r.rec.files_removed, 1u);
    HAC_CHECK_EQ_U64(lines(&r.log, "oldest days removed"), 1u);
    rig_dispose(&r, BOOT_MS);
}

int main(void)
{
    HAC_RUN(the_watch_reports_new_samples_and_new_failures_once);
    HAC_RUN(sources_are_added_with_valid_tags_only);
    HAC_RUN(rows_reach_the_card_at_the_flush_interval);
    HAC_RUN(each_source_has_its_file_and_its_accuracy_column);
    HAC_RUN(a_failure_is_a_row_of_errors);
    HAC_RUN(the_file_turns_at_local_midnight);
    HAC_RUN(rows_without_a_clock_go_to_an_undated_file);
    HAC_RUN(a_closed_file_is_never_reopened);
    HAC_RUN(a_missing_medium_is_a_state_said_once);
    HAC_RUN(a_full_medium_stops_recording_and_is_retried);
    HAC_RUN(a_full_buffer_is_appended_early);
    HAC_RUN(opening_a_file_keeps_data_within_its_bound);
    return hac_test_summary();
}
