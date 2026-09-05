// SPDX-License-Identifier: Apache-2.0
//
// replay_parse_buffer() is exercised directly on in-memory text (also the
// entry point the fuzz target under tests/fuzz drives); replay_source_init()
// additionally needs a real file, written to HAC_TEST_TMP_DIR for the
// duration of one check.

#ifndef HAC_TEST_TMP_DIR
#error "HAC_TEST_TMP_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif
#ifndef HAC_SCENARIOS_DIR
#error "HAC_SCENARIOS_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "hac_test.h"
#include "replay/replay_source.h"

#define SRC_ID 1u

// --- replay_parse_buffer(): the pure parser --------------------------------

static hal_status_t parse_text(const char *text, replay_timeline_t *out)
{
    return replay_parse_buffer(text, strlen(text), out);
}

static void an_empty_file_is_rejected(void)
{
    replay_timeline_t tl;
    HAC_CHECK_EQ_INT(parse_text("", &tl), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_U64(tl.row_count, 0u);  // left zeroed, nothing to dispose
}

static void only_comments_and_blank_lines_is_also_empty(void)
{
    replay_timeline_t tl;
    HAC_CHECK_EQ_INT(parse_text("# just a comment\n\n   \n", &tl), HAL_ERR_INVALID_ARG);
}

static void a_header_with_no_rows_is_rejected(void)
{
    replay_timeline_t tl;
    const char *text = "t_offset_s,co2_ppm,status\n";
    HAC_CHECK_EQ_INT(parse_text(text, &tl), HAL_ERR_INVALID_ARG);
}

static void a_single_row_loads_and_never_loops(void)
{
    replay_timeline_t tl;
    const char *text = "t_offset_s,co2_ppm,status\n0,620,ok\n";
    HAC_CHECK_EQ_INT(parse_text(text, &tl), HAL_OK);

    HAC_CHECK_EQ_U64(tl.row_count, 1u);
    HAC_CHECK(tl.loop_period_s == 0.0);
    HAC_CHECK(tl.field_present[REPLAY_FIELD_CO2_PPM]);
    HAC_CHECK(!tl.field_present[REPLAY_FIELD_TEMPERATURE_C]);

    replay_timeline_dispose(&tl);
}

static void an_unknown_column_is_rejected(void)
{
    replay_timeline_t tl;
    const char *text = "t_offset_s,co2_ppm,pm25_ugm3\n0,620,12\n";
    HAC_CHECK_EQ_INT(parse_text(text, &tl), HAL_ERR_INVALID_ARG);
}

static void a_header_without_t_offset_s_first_is_rejected(void)
{
    replay_timeline_t tl;
    HAC_CHECK_EQ_INT(parse_text("co2_ppm,status\n620,ok\n", &tl), HAL_ERR_INVALID_ARG);
}

static void a_duplicate_column_is_rejected(void)
{
    replay_timeline_t tl;
    const char *text = "t_offset_s,co2_ppm,co2_ppm\n0,620,621\n";
    HAC_CHECK_EQ_INT(parse_text(text, &tl), HAL_ERR_INVALID_ARG);
}

static void a_malformed_number_is_rejected(void)
{
    replay_timeline_t tl;
    HAC_CHECK_EQ_INT(parse_text("t_offset_s,co2_ppm\n0,six-twenty\n", &tl), HAL_ERR_INVALID_ARG);
    HAC_CHECK_EQ_INT(parse_text("t_offset_s,co2_ppm\nzero,620\n", &tl), HAL_ERR_INVALID_ARG);
    // Trailing garbage after an otherwise-valid number must not be ignored.
    HAC_CHECK_EQ_INT(parse_text("t_offset_s,co2_ppm\n0,620ppm\n", &tl), HAL_ERR_INVALID_ARG);
}

static void a_row_with_the_wrong_column_count_is_rejected(void)
{
    replay_timeline_t tl;
    const char *text = "t_offset_s,co2_ppm,status\n0,620\n";  // status column missing on the row
    HAC_CHECK_EQ_INT(parse_text(text, &tl), HAL_ERR_INVALID_ARG);
}

static void an_ok_row_with_a_blank_value_is_rejected(void)
{
    replay_timeline_t tl;
    const char *text = "t_offset_s,co2_ppm,status\n0,,ok\n";
    HAC_CHECK_EQ_INT(parse_text(text, &tl), HAL_ERR_INVALID_ARG);
}

static void a_fault_row_may_leave_its_value_blank(void)
{
    replay_timeline_t tl;
    const char *text = "t_offset_s,co2_ppm,status\n0,,error\n60,620,ok\n";
    HAC_CHECK_EQ_INT(parse_text(text, &tl), HAL_OK);
    HAC_CHECK_EQ_INT(tl.rows[0].status, REPLAY_ROW_ERROR);
    replay_timeline_dispose(&tl);
}

static void every_status_word_is_recognized(void)
{
    replay_timeline_t tl;
    const char *text = "t_offset_s,co2_ppm,status\n"
                       "0,600,ok\n"
                       "10,0,warmup\n"
                       "20,600,stale\n"
                       "30,0,error\n"
                       "40,0,nan\n"
                       "50,9999,out_of_range\n";
    HAC_CHECK_EQ_INT(parse_text(text, &tl), HAL_OK);
    HAC_CHECK_EQ_U64(tl.row_count, 6u);
    HAC_CHECK_EQ_INT(tl.rows[0].status, REPLAY_ROW_OK);
    HAC_CHECK_EQ_INT(tl.rows[1].status, REPLAY_ROW_WARMUP);
    HAC_CHECK_EQ_INT(tl.rows[2].status, REPLAY_ROW_STALE);
    HAC_CHECK_EQ_INT(tl.rows[3].status, REPLAY_ROW_ERROR);
    HAC_CHECK_EQ_INT(tl.rows[4].status, REPLAY_ROW_NAN);
    HAC_CHECK_EQ_INT(tl.rows[5].status, REPLAY_ROW_OUT_OF_RANGE);

    replay_timeline_dispose(&tl);
}

static void an_unrecognized_status_word_is_rejected(void)
{
    replay_timeline_t tl;
    HAC_CHECK_EQ_INT(parse_text("t_offset_s,co2_ppm,status\n0,620,degraded\n", &tl),
                     HAL_ERR_INVALID_ARG);
}

static void comments_and_blank_lines_are_skipped_around_real_rows(void)
{
    replay_timeline_t tl;
    const char *text = "# scenarios/tiny.csv\n"
                       "# t_offset_s -- offset in seconds\n"
                       "t_offset_s,co2_ppm,status\n"
                       "\n"
                       "0,600,ok\n"
                       "   \n"
                       "# a comment mid-file\n"
                       "60,700,ok\n";
    HAC_CHECK_EQ_INT(parse_text(text, &tl), HAL_OK);
    HAC_CHECK_EQ_U64(tl.row_count, 2u);
    HAC_CHECK(tl.loop_period_s == 60.0);

    replay_timeline_dispose(&tl);
}

static void a_missing_column_is_unavailable_for_the_whole_file(void)
{
    replay_timeline_t tl;
    const char *text = "t_offset_s,temperature_c,status\n0,21.5,ok\n";
    HAC_CHECK_EQ_INT(parse_text(text, &tl), HAL_OK);

    HAC_CHECK(tl.field_present[REPLAY_FIELD_TEMPERATURE_C]);
    HAC_CHECK(!tl.field_present[REPLAY_FIELD_HUMIDITY_RH]);
    HAC_CHECK(!tl.field_present[REPLAY_FIELD_CO2_PPM]);

    replay_timeline_dispose(&tl);
}

static void a_file_without_a_status_column_defaults_every_row_to_ok(void)
{
    replay_timeline_t tl;
    HAC_CHECK_EQ_INT(parse_text("t_offset_s,co2_ppm\n0,600\n60,700\n", &tl), HAL_OK);
    HAC_CHECK_EQ_INT(tl.rows[0].status, REPLAY_ROW_OK);
    HAC_CHECK_EQ_INT(tl.rows[1].status, REPLAY_ROW_OK);

    replay_timeline_dispose(&tl);
}

// --- replay_source_init() / playback ---------------------------------------

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

static void loading_a_missing_file_fails_with_not_found(void)
{
    replay_source_t rs;
    hal_status_t st = replay_source_init(&rs, "no/such/file.csv", SENS_CAP_CO2, SRC_ID, NULL, true);
    HAC_CHECK_EQ_INT(st, HAL_ERR_NOT_FOUND);
}

static void interpolation_at_the_midpoint(void)
{
    char *path =
        write_temp_csv("interp_midpoint.csv", "t_offset_s,co2_ppm,status\n0,100,ok\n100,200,ok\n");
    HAC_CHECK(path != NULL);

    adp_clock_virtual_t vc;
    adp_clock_virtual_init(&vc, 1.0);

    replay_source_t rs;
    HAC_CHECK_EQ_INT(
        replay_source_init(&rs, path, SENS_CAP_CO2, SRC_ID, adp_clock_virtual_port(&vc), true),
        HAL_OK);
    HAC_CHECK_EQ_INT(rs.port.init(&rs.port), HAL_OK);
    HAC_CHECK_EQ_INT(rs.port.start(&rs.port), HAL_OK);

    adp_clock_virtual_advance(&vc, 50000u);  // halfway between t=0 and t=100s

    sensor_sample_t sample;
    HAC_CHECK_EQ_INT(rs.port.read(&rs.port, &sample), HAL_OK);
    HAC_CHECK_EQ_INT(sample.co2_ppm.status, VAL_OK);
    HAC_CHECK_EQ_F32(sample.co2_ppm.value, 150.0f, 0.01f);
    HAC_CHECK_EQ_INT(sample.co2_ppm.source_id, SRC_ID);

    rs.port.deinit(&rs.port);
}

static void interpolation_can_be_disabled_for_a_step_function(void)
{
    char *path =
        write_temp_csv("interp_disabled.csv", "t_offset_s,co2_ppm,status\n0,100,ok\n100,200,ok\n");
    HAC_CHECK(path != NULL);

    adp_clock_virtual_t vc;
    adp_clock_virtual_init(&vc, 1.0);

    replay_source_t rs;
    HAC_CHECK_EQ_INT(
        replay_source_init(&rs, path, SENS_CAP_CO2, SRC_ID, adp_clock_virtual_port(&vc), false),
        HAL_OK);
    rs.port.start(&rs.port);

    adp_clock_virtual_advance(&vc, 99000u);  // one second shy of the next row

    sensor_sample_t sample;
    HAC_CHECK_EQ_INT(rs.port.read(&rs.port, &sample), HAL_OK);
    HAC_CHECK_EQ_F32(sample.co2_ppm.value, 100.0f, 0.01f);  // held, not interpolated

    rs.port.deinit(&rs.port);
}

static void loop_wraparound_restarts_at_the_first_row(void)
{
    char *path =
        write_temp_csv("loop_wrap.csv", "t_offset_s,co2_ppm,status\n0,100,ok\n60,200,ok\n");
    HAC_CHECK(path != NULL);

    adp_clock_virtual_t vc;
    adp_clock_virtual_init(&vc, 1.0);

    replay_source_t rs;
    HAC_CHECK_EQ_INT(
        replay_source_init(&rs, path, SENS_CAP_CO2, SRC_ID, adp_clock_virtual_port(&vc), true),
        HAL_OK);
    rs.port.start(&rs.port);

    sensor_sample_t sample;

    // Just short of one full loop: close to row 1 (t=60, value=200).
    adp_clock_virtual_advance(&vc, 59000u);
    HAC_CHECK_EQ_INT(rs.port.read(&rs.port, &sample), HAL_OK);
    HAC_CHECK(sample.co2_ppm.value > 195.0f);

    // Exactly one full loop (t=60s of a 60s loop): back to row 0, not row 1.
    adp_clock_virtual_advance(&vc, 1000u);
    HAC_CHECK_EQ_INT(rs.port.read(&rs.port, &sample), HAL_OK);
    HAC_CHECK_EQ_F32(sample.co2_ppm.value, 100.0f, 0.01f);

    // A little way into the second loop behaves like the same point in the first.
    adp_clock_virtual_advance(&vc, 59000u);
    HAC_CHECK_EQ_INT(rs.port.read(&rs.port, &sample), HAL_OK);
    HAC_CHECK(sample.co2_ppm.value > 195.0f);

    rs.port.deinit(&rs.port);
}

static void a_fault_row_is_reported_and_is_not_interpolated_through(void)
{
    char *path = write_temp_csv("fault_row.csv",
                                "t_offset_s,co2_ppm,status\n0,100,ok\n30,0,error\n60,300,ok\n");
    HAC_CHECK(path != NULL);

    adp_clock_virtual_t vc;
    adp_clock_virtual_init(&vc, 1.0);

    replay_source_t rs;
    HAC_CHECK_EQ_INT(
        replay_source_init(&rs, path, SENS_CAP_CO2, SRC_ID, adp_clock_virtual_port(&vc), true),
        HAL_OK);
    rs.port.start(&rs.port);

    sensor_sample_t sample;

    // Between row 0 (ok) and row 1 (error): held at row 0's value, not eased
    // towards a fault.
    adp_clock_virtual_advance(&vc, 15000u);
    HAC_CHECK_EQ_INT(rs.port.read(&rs.port, &sample), HAL_OK);
    HAC_CHECK_EQ_INT(sample.co2_ppm.status, VAL_OK);
    HAC_CHECK_EQ_F32(sample.co2_ppm.value, 100.0f, 0.01f);

    // Between row 1 (error) and row 2 (ok): the fault itself, value zeroed.
    adp_clock_virtual_advance(&vc, 30000u);
    HAC_CHECK_EQ_INT(rs.port.read(&rs.port, &sample), HAL_OK);
    HAC_CHECK_EQ_INT(sample.co2_ppm.status, VAL_ERROR);
    HAC_CHECK_EQ_F32(sample.co2_ppm.value, 0.0f, 0.0f);

    rs.port.deinit(&rs.port);
}

static void a_nan_row_is_reported_as_an_error_via_measurement_ok(void)
{
    char *path = write_temp_csv("nan_row.csv", "t_offset_s,co2_ppm,status\n0,0,nan\n");
    HAC_CHECK(path != NULL);

    adp_clock_virtual_t vc;
    adp_clock_virtual_init(&vc, 1.0);

    replay_source_t rs;
    HAC_CHECK_EQ_INT(
        replay_source_init(&rs, path, SENS_CAP_CO2, SRC_ID, adp_clock_virtual_port(&vc), true),
        HAL_OK);
    rs.port.start(&rs.port);

    sensor_sample_t sample;
    HAC_CHECK_EQ_INT(rs.port.read(&rs.port, &sample), HAL_OK);
    HAC_CHECK_EQ_INT(sample.co2_ppm.status, VAL_ERROR);

    rs.port.deinit(&rs.port);
}

static void an_absent_column_reads_back_unavailable(void)
{
    char *path = write_temp_csv("co2_only.csv", "t_offset_s,co2_ppm,status\n0,600,ok\n");
    HAC_CHECK(path != NULL);

    adp_clock_virtual_t vc;
    adp_clock_virtual_init(&vc, 1.0);

    replay_source_t rs;
    HAC_CHECK_EQ_INT(
        replay_source_init(&rs, path, SENS_CAP_CO2, SRC_ID, adp_clock_virtual_port(&vc), true),
        HAL_OK);
    rs.port.start(&rs.port);

    sensor_sample_t sample;
    HAC_CHECK_EQ_INT(rs.port.read(&rs.port, &sample), HAL_OK);
    HAC_CHECK_EQ_INT(sample.co2_ppm.status, VAL_OK);
    HAC_CHECK_EQ_INT(sample.temperature_c.status, VAL_UNAVAILABLE);
    HAC_CHECK_EQ_INT(sample.humidity_rh.status, VAL_UNAVAILABLE);

    rs.port.deinit(&rs.port);
}

static void read_before_start_is_not_ready(void)
{
    char *path = write_temp_csv("not_started.csv", "t_offset_s,co2_ppm,status\n0,600,ok\n");
    HAC_CHECK(path != NULL);

    adp_clock_virtual_t vc;
    adp_clock_virtual_init(&vc, 1.0);

    replay_source_t rs;
    HAC_CHECK_EQ_INT(
        replay_source_init(&rs, path, SENS_CAP_CO2, SRC_ID, adp_clock_virtual_port(&vc), true),
        HAL_OK);

    sensor_sample_t sample;
    HAC_CHECK_EQ_INT(rs.port.read(&rs.port, &sample), HAL_ERR_NOT_READY);

    rs.port.deinit(&rs.port);
}

static void two_sources_keep_independent_timelines_and_loop_points(void)
{
    char *co2_path =
        write_temp_csv("multi_co2.csv", "t_offset_s,co2_ppm,status\n0,600,ok\n10,700,ok\n");
    char co2_path_copy[512];
    strcpy(co2_path_copy, co2_path);

    char *env_path = write_temp_csv("multi_env.csv",
                                    "t_offset_s,temperature_c,status\n0,20.0,ok\n100,25.0,ok\n");

    adp_clock_virtual_t vc;
    adp_clock_virtual_init(&vc, 1.0);
    const port_clock_t *clk = adp_clock_virtual_port(&vc);

    replay_source_t co2_src, env_src;
    HAC_CHECK_EQ_INT(replay_source_init(&co2_src, co2_path_copy, SENS_CAP_CO2, 1u, clk, true),
                     HAL_OK);
    HAC_CHECK_EQ_INT(replay_source_init(&env_src, env_path, SENS_CAP_TEMPERATURE, 2u, clk, true),
                     HAL_OK);

    co2_src.port.start(&co2_src.port);
    env_src.port.start(&env_src.port);

    // 15 s in: the 10 s CO2 loop has wrapped once and is 5 s into its second
    // pass (interpolating towards row 1); the 100 s env timeline is nowhere
    // near its own loop point. Neither source's phase leaks into the other's.
    adp_clock_virtual_advance(&vc, 15000u);

    sensor_sample_t co2_sample, env_sample;
    HAC_CHECK_EQ_INT(co2_src.port.read(&co2_src.port, &co2_sample), HAL_OK);
    HAC_CHECK_EQ_INT(env_src.port.read(&env_src.port, &env_sample), HAL_OK);

    HAC_CHECK_EQ_F32(co2_sample.co2_ppm.value, 650.0f, 0.01f);  // 5/10 of the way from 600 to 700
    HAC_CHECK_EQ_F32(env_sample.temperature_c.value, 20.75f,
                     0.01f);  // 15/100 of the way from 20 to 25

    co2_src.port.deinit(&co2_src.port);
    env_src.port.deinit(&env_src.port);
}

// --- the bundled scenario files ---------------------------------------------

static void a_bundled_scenario_file_parses(const char *name, sensor_caps_t caps)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", HAC_SCENARIOS_DIR, name);

    adp_clock_virtual_t vc;
    adp_clock_virtual_init(&vc, 1.0);

    replay_source_t rs;
    hal_status_t st =
        replay_source_init(&rs, path, caps, SRC_ID, adp_clock_virtual_port(&vc), true);
    HAC_CHECK_EQ_INT(st, HAL_OK);
    if (st != HAL_OK) {
        return;
    }
    HAC_CHECK(rs.timeline.row_count > 0u);

    rs.port.start(&rs.port);
    sensor_sample_t sample;
    HAC_CHECK_EQ_INT(rs.port.read(&rs.port, &sample), HAL_OK);

    rs.port.deinit(&rs.port);
}

static void the_co2_spike_meeting_scenario_parses(void)
{
    a_bundled_scenario_file_parses("co2_spike_meeting.csv",
                                   SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY | SENS_CAP_PRESSURE |
                                       SENS_CAP_CO2 | SENS_CAP_GAS_RES);
}

static void the_normal_day_scenario_parses(void)
{
    a_bundled_scenario_file_parses("normal_day.csv", SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY |
                                                         SENS_CAP_PRESSURE | SENS_CAP_GAS_RES);
}

static void the_sensor_scd41_fault_scenario_parses(void)
{
    a_bundled_scenario_file_parses("sensor_scd41_fault.csv", SENS_CAP_CO2);
}

// battery_drain.csv and wifi_flapping.csv are reserved for a future
// adp_power_sim / adp_net_sim, not for replay_source: their columns
// (battery_percent, net_up, ...) are deliberately outside what a
// sensor_source_t can report, so loading them here must fail loudly rather
// than silently produce a source with no fields.
static void the_power_and_net_scenario_files_are_not_sensor_files(void)
{
    replay_timeline_t tl;
    char path[512];

    snprintf(path, sizeof(path), "%s/battery_drain.csv", HAC_SCENARIOS_DIR);
    HAC_CHECK_EQ_INT(replay_parse_file(path, &tl), HAL_ERR_INVALID_ARG);

    snprintf(path, sizeof(path), "%s/wifi_flapping.csv", HAC_SCENARIOS_DIR);
    HAC_CHECK_EQ_INT(replay_parse_file(path, &tl), HAL_ERR_INVALID_ARG);
}

int main(void)
{
    HAC_RUN(an_empty_file_is_rejected);
    HAC_RUN(only_comments_and_blank_lines_is_also_empty);
    HAC_RUN(a_header_with_no_rows_is_rejected);
    HAC_RUN(a_single_row_loads_and_never_loops);
    HAC_RUN(an_unknown_column_is_rejected);
    HAC_RUN(a_header_without_t_offset_s_first_is_rejected);
    HAC_RUN(a_duplicate_column_is_rejected);
    HAC_RUN(a_malformed_number_is_rejected);
    HAC_RUN(a_row_with_the_wrong_column_count_is_rejected);
    HAC_RUN(an_ok_row_with_a_blank_value_is_rejected);
    HAC_RUN(a_fault_row_may_leave_its_value_blank);
    HAC_RUN(every_status_word_is_recognized);
    HAC_RUN(an_unrecognized_status_word_is_rejected);
    HAC_RUN(comments_and_blank_lines_are_skipped_around_real_rows);
    HAC_RUN(a_missing_column_is_unavailable_for_the_whole_file);
    HAC_RUN(a_file_without_a_status_column_defaults_every_row_to_ok);

    HAC_RUN(loading_a_missing_file_fails_with_not_found);
    HAC_RUN(interpolation_at_the_midpoint);
    HAC_RUN(interpolation_can_be_disabled_for_a_step_function);
    HAC_RUN(loop_wraparound_restarts_at_the_first_row);
    HAC_RUN(a_fault_row_is_reported_and_is_not_interpolated_through);
    HAC_RUN(a_nan_row_is_reported_as_an_error_via_measurement_ok);
    HAC_RUN(an_absent_column_reads_back_unavailable);
    HAC_RUN(read_before_start_is_not_ready);
    HAC_RUN(two_sources_keep_independent_timelines_and_loop_points);

    HAC_RUN(the_co2_spike_meeting_scenario_parses);
    HAC_RUN(the_normal_day_scenario_parses);
    HAC_RUN(the_sensor_scd41_fault_scenario_parses);
    HAC_RUN(the_power_and_net_scenario_files_are_not_sensor_files);
    return hac_test_summary();
}
