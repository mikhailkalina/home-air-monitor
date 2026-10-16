// SPDX-License-Identifier: Apache-2.0
//
// core/recording/record_format.c: the header and the rows of a recording,
// byte for byte, and -- what the format is for -- that the simulator's
// parser (platform/host/replay/replay_source.h) reads every one of them
// back to the same statuses, accuracies and values, within the decimals
// written.

#include "recording/record_format.h"

#include <math.h>
#include <string.h>

#include "hac_test.h"
#include "recording/record_text.h"
#include "replay/replay_source.h"

#define SCD4X_CAPS ((sensor_caps_t)(SENS_CAP_CO2 | SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY))
#define BSEC_CAPS                                                                                  \
    ((sensor_caps_t)(SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY | SENS_CAP_PRESSURE | SENS_CAP_IAQ | \
                     SENS_CAP_GAS_RES))
#define ALL_CAPS                                                                                   \
    ((sensor_caps_t)(SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY | SENS_CAP_PRESSURE | SENS_CAP_CO2 | \
                     SENS_CAP_IAQ | SENS_CAP_VOC | SENS_CAP_GAS_RES))

#define ID 2u
#define TS 1000u
#define UTC_2026_10_16_12 1792152000000ull  // 2026-10-16T12:00:00Z

static measurement_f32_t ok(float v)
{
    return measurement_ok(v, TS, ID);
}

static measurement_f32_t st(value_status_t s)
{
    return measurement_status(s, TS, ID);
}

static sensor_sample_t scd4x_sample(void)
{
    sensor_sample_t s;
    memset(&s, 0, sizeof(s));
    s.co2_ppm = ok(812.0f);
    s.temperature_c = ok(24.125f);
    s.humidity_rh = ok(47.03f);
    return s;
}

// --- text --------------------------------------------------------------------

static void fixed_point_rounds_half_away_and_never_writes_negative_zero(void)
{
    char buf[32];
    HAC_CHECK_EQ_U64(record_text_fixed(buf, sizeof(buf), 0u, 21.5, 2u), 5u);
    HAC_CHECK(strcmp(buf, "21.50") == 0);
    (void)record_text_fixed(buf, sizeof(buf), 0u, -5.255, 1u);
    HAC_CHECK(strcmp(buf, "-5.3") == 0);
    (void)record_text_fixed(buf, sizeof(buf), 0u, -0.004, 2u);
    HAC_CHECK(strcmp(buf, "0.00") == 0);
    (void)record_text_fixed(buf, sizeof(buf), 0u, 0.4, 0u);
    HAC_CHECK(strcmp(buf, "0") == 0);
    (void)record_text_fixed(buf, sizeof(buf), 0u, 1006.0625, 3u);
    HAC_CHECK(strcmp(buf, "1006.063") == 0);
    (void)record_text_fixed(buf, sizeof(buf), 0u, 0.05, 2u);
    HAC_CHECK(strcmp(buf, "0.05") == 0);

    // Not a number, or too large: nothing a reader could mistake for one.
    HAC_CHECK_EQ_U64(record_text_fixed(buf, sizeof(buf), 0u, NAN, 2u), sizeof(buf));
    HAC_CHECK_EQ_U64(record_text_fixed(buf, sizeof(buf), 0u, 1e20, 0u), sizeof(buf));
    // Out of room: the cap, never a partial number.
    HAC_CHECK_EQ_U64(record_text_fixed(buf, 4u, 0u, 1234.5, 1u), 4u);
}

static void utc_is_written_to_the_second_or_the_millisecond(void)
{
    char buf[32];
    size_t n = record_format_utc(buf, sizeof(buf), 0u, UTC_2026_10_16_12 + 9876u, false);
    HAC_CHECK_EQ_U64(n, RECORD_UTC_LEN);
    HAC_CHECK(strcmp(buf, "2026-10-16T12:00:09Z") == 0);
    n = record_format_utc(buf, sizeof(buf), 0u, UTC_2026_10_16_12 + 9876u, true);
    HAC_CHECK_EQ_U64(n, RECORD_UTC_MS_LEN);
    HAC_CHECK(strcmp(buf, "2026-10-16T12:00:09.876Z") == 0);
}

static void accuracy_words_go_both_ways(void)
{
    for (unsigned a = ACC_NOT_REPORTED; a <= ACC_HIGH; ++a) {
        const char *word = record_accuracy_word((value_accuracy_t)a);
        value_accuracy_t back = ACC_HIGH;
        HAC_CHECK(record_accuracy_from_word(word, strlen(word), &back));
        HAC_CHECK_EQ_INT(back, (int)a);
    }
    value_accuracy_t out;
    HAC_CHECK(!record_accuracy_from_word("good", 4u, &out));
    HAC_CHECK(!record_accuracy_from_word("lo", 2u, &out));
}

// --- header and rows -----------------------------------------------------------

static void the_header_names_the_claimed_fields_in_field_order(void)
{
    char buf[RECORD_LINE_MAX];
    HAC_CHECK(record_header(buf, sizeof(buf), SCD4X_CAPS, 0) > 0u);
    HAC_CHECK(strcmp(buf, "t_offset_s,utc,temperature_c,humidity_rh,co2_ppm,status,"
                          "temperature_c_status,humidity_rh_status,co2_ppm_status\n") == 0);

    // An accuracy column only where asked, and only for a claimed field.
    HAC_CHECK(record_header(buf, sizeof(buf), BSEC_CAPS, SENS_CAP_IAQ | SENS_CAP_CO2) > 0u);
    HAC_CHECK(strcmp(buf, "t_offset_s,utc,temperature_c,humidity_rh,pressure_hpa,iaq,gas_res_ohm,"
                          "status,temperature_c_status,humidity_rh_status,pressure_hpa_status,"
                          "iaq_status,gas_res_ohm_status,iaq_acc\n") == 0);

    // Too small a buffer: nothing.
    HAC_CHECK_EQ_U64(record_header(buf, 20u, SCD4X_CAPS, 0), 0u);
}

static void the_preamble_names_the_source(void)
{
    char buf[RECORD_LINE_MAX];
    HAC_CHECK(record_preamble(buf, sizeof(buf), "scd4x", 1u) > 0u);
    HAC_CHECK(strcmp(buf, "# home-air-monitor recording: source scd4x, id 1\n") == 0);
}

static void an_ok_row_has_values_and_no_overrides(void)
{
    char buf[RECORD_LINE_MAX];
    const sensor_sample_t s = scd4x_sample();
    HAC_CHECK(record_row(buf, sizeof(buf), &s, SCD4X_CAPS, 0, 12345u, UTC_2026_10_16_12) > 0u);
    HAC_CHECK(strcmp(buf, "12.345,2026-10-16T12:00:00Z,24.13,47.03,812,ok,,,\n") == 0);
}

static void a_row_without_a_wall_time_leaves_utc_empty(void)
{
    char buf[RECORD_LINE_MAX];
    const sensor_sample_t s = scd4x_sample();
    HAC_CHECK(record_row(buf, sizeof(buf), &s, SCD4X_CAPS, 0, 0u, 0u) > 0u);
    HAC_CHECK(strcmp(buf, "0.000,,24.13,47.03,812,ok,,,\n") == 0);
}

static void a_warming_field_is_an_override_with_no_value(void)
{
    char buf[RECORD_LINE_MAX];
    sensor_sample_t s;
    memset(&s, 0, sizeof(s));
    s.temperature_c = ok(23.41f);
    s.humidity_rh = ok(48.2f);
    s.pressure_hpa = ok(1006.214f);
    s.iaq = st(VAL_WARMUP);
    s.iaq.accuracy = ACC_UNRELIABLE;
    s.gas_resistance_ohm = ok(153204.0f);
    HAC_CHECK(record_row(buf, sizeof(buf), &s, BSEC_CAPS, SENS_CAP_IAQ, 0u, UTC_2026_10_16_12) >
              0u);
    HAC_CHECK(strcmp(buf, "0.000,2026-10-16T12:00:00Z,23.41,48.20,1006.214,,153204,ok,,,,warmup,,"
                          "unreliable\n") == 0);
}

static void a_failed_source_is_a_row_of_errors(void)
{
    char buf[RECORD_LINE_MAX];
    sensor_sample_t s;
    memset(&s, 0, sizeof(s));
    s.co2_ppm = st(VAL_ERROR);
    s.temperature_c = st(VAL_ERROR);
    s.humidity_rh = st(VAL_ERROR);
    HAC_CHECK(record_row(buf, sizeof(buf), &s, SCD4X_CAPS, 0, 5000u, 0u) > 0u);
    HAC_CHECK(strcmp(buf, "5.000,,,,,error,,,\n") == 0);
}

static void a_stale_value_keeps_its_number(void)
{
    char buf[RECORD_LINE_MAX];
    sensor_sample_t s = scd4x_sample();
    s.co2_ppm.status = VAL_STALE;
    HAC_CHECK(record_row(buf, sizeof(buf), &s, SCD4X_CAPS, 0, 0u, 0u) > 0u);
    HAC_CHECK(strcmp(buf, "0.000,,24.13,47.03,812,ok,,,stale\n") == 0);
}

static void a_row_that_does_not_fit_is_not_written(void)
{
    char buf[24];
    const sensor_sample_t s = scd4x_sample();
    HAC_CHECK_EQ_U64(record_row(buf, sizeof(buf), &s, SCD4X_CAPS, 0, 0u, UTC_2026_10_16_12), 0u);
}

static void the_sample_time_is_the_latest_claimed_field(void)
{
    sensor_sample_t s = scd4x_sample();
    s.humidity_rh.ts_ms = TS + 7u;
    s.pressure_hpa = measurement_ok(1.0f, TS + 100u, ID);  // not claimed: ignored
    HAC_CHECK_EQ_U64(record_sample_time_ms(&s, SCD4X_CAPS), TS + 7u);
    sensor_sample_t none;
    memset(&none, 0, sizeof(none));
    HAC_CHECK_EQ_U64(record_sample_time_ms(&none, SCD4X_CAPS), 0u);
}

// --- what the parser makes of it -----------------------------------------------

static const measurement_f32_t *field_of(const sensor_sample_t *s, sensor_field_t f)
{
    switch (f) {
        case SENSOR_FIELD_TEMPERATURE:
            return &s->temperature_c;
        case SENSOR_FIELD_HUMIDITY:
            return &s->humidity_rh;
        case SENSOR_FIELD_PRESSURE:
            return &s->pressure_hpa;
        case SENSOR_FIELD_CO2:
            return &s->co2_ppm;
        case SENSOR_FIELD_IAQ:
            return &s->iaq;
        case SENSOR_FIELD_VOC_INDEX:
            return &s->voc_index;
        case SENSOR_FIELD_GAS_RESISTANCE:
        default:
            return &s->gas_resistance_ohm;
    }
}

// Every status on every field, every accuracy, values of either sign: the
// parser gives back what the formatter was given, to half the last decimal.
static void the_parser_reads_back_what_the_formatter_writes(void)
{
    static const value_status_t STATUSES[] = {VAL_OK, VAL_WARMUP, VAL_STALE, VAL_ERROR,
                                              VAL_UNAVAILABLE};
    static const float VALUES[SENSOR_FIELD_COUNT] = {-12.345f, 47.035f, 1006.0625f, 812.4f,
                                                     50.004f,  112.25f, 153204.5f};
    enum { ROWS = 6 };

    char text[ROWS * RECORD_LINE_MAX + RECORD_LINE_MAX];
    size_t len = record_header(text, sizeof(text), ALL_CAPS, ALL_CAPS);
    sensor_sample_t written[ROWS];
    for (unsigned r = 0u; r < ROWS; ++r) {
        sensor_sample_t s;
        memset(&s, 0, sizeof(s));
        for (unsigned f = 0u; f < SENSOR_FIELD_COUNT; ++f) {
            measurement_f32_t *m = (measurement_f32_t *)field_of(&s, (sensor_field_t)f);
            const value_status_t status = STATUSES[(r + f) % 5u];
            *m = (status == VAL_OK || status == VAL_STALE) ? ok(VALUES[f] + (float)r) : st(status);
            m->status = status;
            m->accuracy = (value_accuracy_t)((r + 2u * f) % 5u);
        }
        written[r] = s;
        const size_t n = record_row(text + len, sizeof(text) - len, &s, ALL_CAPS, ALL_CAPS,
                                    (uint64_t)r * 5000u, UTC_2026_10_16_12 + r * 5000u);
        HAC_CHECK(n > 0u);
        len += n;
    }

    replay_timeline_t tl;
    HAC_CHECK_EQ_INT(replay_parse_buffer(text, len, &tl), HAL_OK);
    HAC_CHECK_EQ_U64(tl.row_count, (uint64_t)ROWS);
    HAC_CHECK_EQ_INT((int)replay_timeline_caps(&tl), (int)ALL_CAPS);
    HAC_CHECK_EQ_INT((int)replay_timeline_accuracy_caps(&tl), (int)ALL_CAPS);
    HAC_CHECK_EQ_U64(tl.first_utc_ms, UTC_2026_10_16_12);

    // Playback loops at the last row, so it is read at its predecessors'
    // times only; the first five rows give every field every status.
    for (unsigned r = 0u; r + 1u < ROWS && r < tl.row_count; ++r) {
        sensor_sample_t back;
        replay_timeline_sample(&tl, (uint64_t)r * 5000u, false, TS, ID, &back);
        for (unsigned f = 0u; f < SENSOR_FIELD_COUNT; ++f) {
            const measurement_f32_t *w = field_of(&written[r], (sensor_field_t)f);
            const measurement_f32_t *b = field_of(&back, (sensor_field_t)f);
            HAC_CHECK_EQ_INT(tl.rows[r].accuracy[f], (int)w->accuracy);
            HAC_CHECK_EQ_INT(b->accuracy, (int)w->accuracy);
            if (w->status == VAL_OK) {
                HAC_CHECK_EQ_INT(b->status, VAL_OK);
                const float half = 0.5f / powf(10.0f, (float)record_decimals((sensor_field_t)f));
                HAC_CHECK_EQ_F32(b->value, w->value, half + 1e-3f);
            } else {
                // Stale carries its number in the file; playback reports the
                // status alone, as it always has for a stale row.
                HAC_CHECK_EQ_INT(b->status, (int)w->status);
            }
        }
    }
    replay_timeline_dispose(&tl);
}

int main(void)
{
    HAC_RUN(fixed_point_rounds_half_away_and_never_writes_negative_zero);
    HAC_RUN(utc_is_written_to_the_second_or_the_millisecond);
    HAC_RUN(accuracy_words_go_both_ways);
    HAC_RUN(the_header_names_the_claimed_fields_in_field_order);
    HAC_RUN(the_preamble_names_the_source);
    HAC_RUN(an_ok_row_has_values_and_no_overrides);
    HAC_RUN(a_row_without_a_wall_time_leaves_utc_empty);
    HAC_RUN(a_warming_field_is_an_override_with_no_value);
    HAC_RUN(a_failed_source_is_a_row_of_errors);
    HAC_RUN(a_stale_value_keeps_its_number);
    HAC_RUN(a_row_that_does_not_fit_is_not_written);
    HAC_RUN(the_sample_time_is_the_latest_claimed_field);
    HAC_RUN(the_parser_reads_back_what_the_formatter_writes);
    return hac_test_summary();
}
