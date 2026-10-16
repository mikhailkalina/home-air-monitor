// SPDX-License-Identifier: Apache-2.0
//
// core/recording/log_file.c over adp_files_dir: the debug log's lines in
// log/<day>.log with their UTC time, an undated file before the clock, a day
// file appended to across boots, the lines a missing card cost said in the
// file once it is back, and the bound.

#ifndef HAC_TEST_TMP_DIR
#error "HAC_TEST_TMP_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif

#define _POSIX_C_SOURCE 200809L

#include "recording/log_file.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "adp_files_dir.h"
#include "hac_test.h"
#include "recording/record_limits.h"

#define ROOT HAC_TEST_TMP_DIR "/log_file"
#define NOON_UTC 1792152000000ull  // 2026-10-16T12:00:00Z, 15:00 in Vilnius
#define MIDNIGHT_UTC (NOON_UTC + 9ull * 3600000ull)

static adp_files_dir_t s_dir;
static log_file_t s_lf;
static time_zone_t s_zone;

static void wipe(void)
{
    DIR *d = opendir(ROOT "/log");
    if (d == NULL) {
        return;
    }
    for (struct dirent *e = readdir(d); e != NULL; e = readdir(d)) {
        if (e->d_name[0] != '.') {
            char path[512];
            snprintf(path, sizeof(path), "%s/log/%s", ROOT, e->d_name);
            (void)unlink(path);
        }
    }
    closedir(d);
}

static void setup(void)
{
    HAC_CHECK_EQ_INT(adp_files_dir_init(&s_dir, ROOT), HAL_OK);
    wipe();
    s_zone = time_zone_eu(120);
    log_file_init(&s_lf, adp_files_dir_port(&s_dir), &s_zone);
}

static void teardown(uint64_t now_ms)
{
    log_file_close(&s_lf, now_ms);
    adp_files_dir_deinit(&s_dir);
}

static void line(uint64_t now_ms, uint64_t wall_ms, const char *text)
{
    log_file_write(&s_lf, now_ms, wall_ms, text, strlen(text));
}

static const char *read_file(const char *rel)
{
    static char buf[8192];
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", ROOT, rel);
    FILE *f = fopen(path, "rb");
    buf[0] = '\0';
    if (f != NULL) {
        buf[fread(buf, 1u, sizeof(buf) - 1u, f)] = '\0';
        fclose(f);
    }
    return buf;
}

static void lines_carry_their_utc_time_to_the_millisecond(void)
{
    setup();
    line(1000u, NOON_UTC + 123u, "I (1000) main: one\n");
    line(1500u, NOON_UTC + 623u, "W (1500) main: two\n");
    HAC_CHECK(strcmp(read_file("log/2026-10-16.log"), "") == 0);  // buffered
    log_file_tick(&s_lf, 1000u + RECORD_LOG_FLUSH_INTERVAL_MS);
    HAC_CHECK(strcmp(read_file("log/2026-10-16.log"),
                     "2026-10-16T12:00:00.123Z I (1000) main: one\n"
                     "2026-10-16T12:00:00.623Z W (1500) main: two\n") == 0);
    HAC_CHECK_EQ_U64(s_lf.lines_written, 2u);
    HAC_CHECK_EQ_INT(s_lf.state, RECORDER_ON);
    teardown(20000u);
}

static void without_a_clock_lines_go_undated_and_bare(void)
{
    setup();
    line(100u, 0u, "I (100) main: early\n");
    line(900u, NOON_UTC, "I (900) main: the clock is set\n");
    log_file_flush(&s_lf, 900u);
    HAC_CHECK(strcmp(read_file("log/undated-0001.log"), "I (100) main: early\n") == 0);
    HAC_CHECK(strcmp(read_file("log/2026-10-16.log"),
                     "2026-10-16T12:00:00.000Z I (900) main: the clock is set\n") == 0);
    teardown(900u);
}

static void a_day_is_one_file_across_boots_and_turns_at_local_midnight(void)
{
    setup();
    line(100u, MIDNIGHT_UTC - 2000u, "I (100) main: boot one\n");
    teardown(100u);

    HAC_CHECK_EQ_INT(adp_files_dir_init(&s_dir, ROOT), HAL_OK);  // no wipe: the same card
    log_file_init(&s_lf, adp_files_dir_port(&s_dir), &s_zone);
    line(50u, MIDNIGHT_UTC - 1000u, "I (50) main: boot two\n");
    line(1050u, MIDNIGHT_UTC, "I (1050) main: after midnight\n");
    HAC_CHECK(strcmp(read_file("log/2026-10-16.log"),
                     "2026-10-16T20:59:58.000Z I (100) main: boot one\n"
                     "2026-10-16T20:59:59.000Z I (50) main: boot two\n") == 0);
    log_file_flush(&s_lf, 1050u);
    HAC_CHECK(strcmp(read_file("log/2026-10-17.log"),
                     "2026-10-16T21:00:00.000Z I (1050) main: after midnight\n") == 0);
    teardown(1050u);
}

static void lines_lost_to_a_missing_card_are_said_once_it_is_back(void)
{
    setup();
    adp_files_dir_set_present(&s_dir, false);
    line(100u, NOON_UTC, "I (100) a\n");
    line(200u, NOON_UTC + 100u, "I (200) b\n");
    HAC_CHECK_EQ_INT(s_lf.state, RECORDER_NO_MEDIUM);
    HAC_CHECK_EQ_U64(s_lf.lines_dropped, 2u);

    adp_files_dir_set_present(&s_dir, true);
    line(300u, NOON_UTC + 200u, "I (300) c\n");
    log_file_flush(&s_lf, 300u);
    HAC_CHECK(strcmp(read_file("log/2026-10-16.log"),
                     "2026-10-16T12:00:00.200Z log: 2 lines lost while the card could not be "
                     "written\n"
                     "2026-10-16T12:00:00.200Z I (300) c\n") == 0);
    teardown(300u);
}

static void an_overflowing_buffer_is_noted_in_the_file(void)
{
    setup();
    line(100u, NOON_UTC, "I (100) a\n");
    log_file_note_lost(&s_lf, 200u, NOON_UTC + 100u, 3u, "the log's buffer in RAM was full");
    log_file_note_lost(&s_lf, 200u, NOON_UTC + 100u, 0u, "nothing");  // nothing
    log_file_flush(&s_lf, 200u);
    HAC_CHECK(strcmp(read_file("log/2026-10-16.log"),
                     "2026-10-16T12:00:00.000Z I (100) a\n"
                     "2026-10-16T12:00:00.100Z log: 3 lines lost before this one: the log's "
                     "buffer in RAM was full\n") == 0);
    HAC_CHECK_EQ_U64(s_lf.lines_dropped, 3u);
    teardown(200u);
}

static void a_full_card_stops_the_log_until_the_retry(void)
{
    setup();
    adp_files_dir_set_limit(&s_dir, 1u);
    line(100u, NOON_UTC, "I (100) a\n");
    log_file_flush(&s_lf, 100u);
    HAC_CHECK_EQ_INT(s_lf.state, RECORDER_FULL);
    line(200u, NOON_UTC, "I (200) b\n");  // before the retry: lost
    adp_files_dir_set_limit(&s_dir, 0u);
    line(100u + RECORD_RETRY_MS, NOON_UTC, "I (60100) c\n");
    log_file_flush(&s_lf, 100u + RECORD_RETRY_MS);
    HAC_CHECK_EQ_INT(s_lf.state, RECORDER_ON);
    HAC_CHECK(strstr(read_file("log/2026-10-16.log"), "log: 2 lines lost") != NULL);
    teardown(100u + RECORD_RETRY_MS);
}

static void the_log_is_kept_within_its_bound(void)
{
    setup();
    s_lf.dir_cap_bytes = 100u;
    port_files_t *f = adp_files_dir_port(&s_dir);
    port_file_t *old = NULL;
    HAC_CHECK_EQ_INT(f->open_append(f, "log/2026-10-14.log", &old), HAL_OK);
    char filler[120];
    memset(filler, 'x', sizeof(filler));
    HAC_CHECK_EQ_INT(f->append(f, old, filler, sizeof(filler)), HAL_OK);
    HAC_CHECK_EQ_INT(f->close(f, old), HAL_OK);
    line(100u, NOON_UTC, "I (100) a\n");
    HAC_CHECK(access(ROOT "/log/2026-10-14.log", F_OK) != 0);
    HAC_CHECK_EQ_U64(s_lf.files_removed, 1u);
    teardown(100u);
}

int main(void)
{
    HAC_RUN(lines_carry_their_utc_time_to_the_millisecond);
    HAC_RUN(without_a_clock_lines_go_undated_and_bare);
    HAC_RUN(a_day_is_one_file_across_boots_and_turns_at_local_midnight);
    HAC_RUN(lines_lost_to_a_missing_card_are_said_once_it_is_back);
    HAC_RUN(an_overflowing_buffer_is_noted_in_the_file);
    HAC_RUN(a_full_card_stops_the_log_until_the_retry);
    HAC_RUN(the_log_is_kept_within_its_bound);
    return hac_test_summary();
}
