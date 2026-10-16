// SPDX-License-Identifier: Apache-2.0

#include "recording/record_format.h"

#include <string.h>

#include "domain/civil_time.h"
#include "recording/record_text.h"

#define MS_PER_SECOND 1000u
#define YEAR_DIGITS 4u
#define TWO_DIGITS 2u
#define MS_DIGITS 3u

typedef struct {
    const char *column;
    sensor_caps_t cap;
    // CHOICE: the decimals each field is written with, at or finer than what
    // the sensors resolve, so a replayed recording shows what the device
    // showed: the BME68x's temperature and humidity to 0.01 (its
    // compensation's own resolution), pressure to 0.001 hPa -- the 0.07 hPa
    // movements seen when a window closed (board_notes.md) stay readable --
    // CO2 in whole ppm (the SCD4x reports nothing finer), IAQ to 0.01, the
    // VOC index to 0.1, gas resistance in whole ohms.
    unsigned decimals;
} field_info_t;

static const field_info_t FIELDS[SENSOR_FIELD_COUNT] = {
    [SENSOR_FIELD_TEMPERATURE] = {"temperature_c", SENS_CAP_TEMPERATURE, 2u},
    [SENSOR_FIELD_HUMIDITY] = {"humidity_rh", SENS_CAP_HUMIDITY, 2u},
    [SENSOR_FIELD_PRESSURE] = {"pressure_hpa", SENS_CAP_PRESSURE, 3u},
    [SENSOR_FIELD_CO2] = {"co2_ppm", SENS_CAP_CO2, 0u},
    [SENSOR_FIELD_IAQ] = {"iaq", SENS_CAP_IAQ, 2u},
    [SENSOR_FIELD_VOC_INDEX] = {"voc_index", SENS_CAP_VOC, 1u},
    [SENSOR_FIELD_GAS_RESISTANCE] = {"gas_res_ohm", SENS_CAP_GAS_RES, 0u},
};

const char *record_column_name(sensor_field_t field)
{
    return ((unsigned)field < (unsigned)SENSOR_FIELD_COUNT) ? FIELDS[field].column : NULL;
}

unsigned record_decimals(sensor_field_t field)
{
    return ((unsigned)field < (unsigned)SENSOR_FIELD_COUNT) ? FIELDS[field].decimals : 0u;
}

sensor_caps_t record_field_cap(sensor_field_t field)
{
    return ((unsigned)field < (unsigned)SENSOR_FIELD_COUNT) ? FIELDS[field].cap : (sensor_caps_t)0;
}

const char *record_status_word(value_status_t status)
{
    switch (status) {
        case VAL_OK:
            return "ok";
        case VAL_WARMUP:
            return "warmup";
        case VAL_STALE:
            return "stale";
        case VAL_ERROR:
            return "error";
        case VAL_UNAVAILABLE:
        default:
            return "unavailable";
    }
}

const char *record_accuracy_word(value_accuracy_t accuracy)
{
    switch (accuracy) {
        case ACC_UNRELIABLE:
            return "unreliable";
        case ACC_LOW:
            return "low";
        case ACC_MEDIUM:
            return "medium";
        case ACC_HIGH:
            return "high";
        case ACC_NOT_REPORTED:
        default:
            return "";
    }
}

bool record_accuracy_from_word(const char *word, size_t len, value_accuracy_t *out)
{
    for (unsigned a = (unsigned)ACC_NOT_REPORTED; a <= (unsigned)ACC_HIGH; ++a) {
        const char *candidate = record_accuracy_word((value_accuracy_t)a);
        if (strlen(candidate) == len && (len == 0u || memcmp(candidate, word, len) == 0)) {
            *out = (value_accuracy_t)a;
            return true;
        }
    }
    return false;
}

size_t record_format_utc(char *dst, size_t cap, size_t len, uint64_t unix_ms, bool with_ms)
{
    const civil_time_t t = civil_time_from_unix_ms(unix_ms);
    len = record_text_u64(dst, cap, len, (uint64_t)t.year, YEAR_DIGITS);
    len = record_text_char(dst, cap, len, '-');
    len = record_text_u64(dst, cap, len, t.month, TWO_DIGITS);
    len = record_text_char(dst, cap, len, '-');
    len = record_text_u64(dst, cap, len, t.day, TWO_DIGITS);
    len = record_text_char(dst, cap, len, 'T');
    len = record_text_u64(dst, cap, len, t.hour, TWO_DIGITS);
    len = record_text_char(dst, cap, len, ':');
    len = record_text_u64(dst, cap, len, t.minute, TWO_DIGITS);
    len = record_text_char(dst, cap, len, ':');
    len = record_text_u64(dst, cap, len, t.second, TWO_DIGITS);
    if (with_ms) {
        len = record_text_char(dst, cap, len, '.');
        len = record_text_u64(dst, cap, len, unix_ms % MS_PER_SECOND, MS_DIGITS);
    }
    return record_text_char(dst, cap, len, 'Z');
}

size_t record_preamble(char *dst, size_t cap, const char *tag, uint8_t source_id)
{
    size_t len = record_text_str(dst, cap, 0u, "# home-air-monitor recording: source ");
    len = record_text_str(dst, cap, len, tag);
    len = record_text_str(dst, cap, len, ", id ");
    len = record_text_u64(dst, cap, len, source_id, 1u);
    len = record_text_char(dst, cap, len, '\n');
    return record_text_overflowed(len, cap) ? 0u : len;
}

static bool claims(sensor_caps_t caps, unsigned f)
{
    return (caps & FIELDS[f].cap) != 0u;
}

// The columns that follow the values, one per claimed field: "<field><suffix>".
static size_t append_suffixed(char *dst, size_t cap, size_t len, sensor_caps_t caps,
                              const char *suffix)
{
    for (unsigned f = 0u; f < (unsigned)SENSOR_FIELD_COUNT; ++f) {
        if (claims(caps, f)) {
            len = record_text_char(dst, cap, len, ',');
            len = record_text_str(dst, cap, len, FIELDS[f].column);
            len = record_text_str(dst, cap, len, suffix);
        }
    }
    return len;
}

size_t record_header(char *dst, size_t cap, sensor_caps_t caps, sensor_caps_t accuracy_caps)
{
    size_t len = record_text_str(dst, cap, 0u, "t_offset_s,utc");
    for (unsigned f = 0u; f < (unsigned)SENSOR_FIELD_COUNT; ++f) {
        if (claims(caps, f)) {
            len = record_text_char(dst, cap, len, ',');
            len = record_text_str(dst, cap, len, FIELDS[f].column);
        }
    }
    len = record_text_str(dst, cap, len, ",status");
    len = append_suffixed(dst, cap, len, caps, "_status");
    len = append_suffixed(dst, cap, len, (sensor_caps_t)(caps & accuracy_caps), "_acc");
    len = record_text_char(dst, cap, len, '\n');
    return record_text_overflowed(len, cap) ? 0u : len;
}

static const measurement_f32_t *field_of(const sensor_sample_t *s, unsigned f)
{
    switch ((sensor_field_t)f) {
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

uint64_t record_sample_time_ms(const sensor_sample_t *sample, sensor_caps_t caps)
{
    uint64_t latest = 0u;
    for (unsigned f = 0u; f < (unsigned)SENSOR_FIELD_COUNT; ++f) {
        if (claims(caps, f) && field_of(sample, f)->ts_ms > latest) {
            latest = field_of(sample, f)->ts_ms;
        }
    }
    return latest;
}

size_t record_row(char *dst, size_t cap, const sensor_sample_t *sample, sensor_caps_t caps,
                  sensor_caps_t accuracy_caps, uint64_t t_offset_ms, uint64_t utc_ms)
{
    size_t len = record_text_u64(dst, cap, 0u, t_offset_ms / MS_PER_SECOND, 1u);
    len = record_text_char(dst, cap, len, '.');
    len = record_text_u64(dst, cap, len, t_offset_ms % MS_PER_SECOND, MS_DIGITS);
    len = record_text_char(dst, cap, len, ',');
    if (utc_ms != 0u) {
        len = record_format_utc(dst, cap, len, utc_ms, false);
    }

    // The values; the row's status is the first claimed field's.
    bool have_row_status = false;
    value_status_t row_status = VAL_UNAVAILABLE;
    for (unsigned f = 0u; f < (unsigned)SENSOR_FIELD_COUNT; ++f) {
        if (!claims(caps, f)) {
            continue;
        }
        const measurement_f32_t *m = field_of(sample, f);
        if (!have_row_status) {
            row_status = m->status;
            have_row_status = true;
        }
        len = record_text_char(dst, cap, len, ',');
        if (measurement_has_value(m)) {
            len = record_text_fixed(dst, cap, len, (double)m->value, FIELDS[f].decimals);
        }
    }
    len = record_text_char(dst, cap, len, ',');
    len = record_text_str(dst, cap, len, record_status_word(row_status));

    for (unsigned f = 0u; f < (unsigned)SENSOR_FIELD_COUNT; ++f) {
        if (claims(caps, f)) {
            const value_status_t status = field_of(sample, f)->status;
            len = record_text_char(dst, cap, len, ',');
            if (status != row_status) {
                len = record_text_str(dst, cap, len, record_status_word(status));
            }
        }
    }
    for (unsigned f = 0u; f < (unsigned)SENSOR_FIELD_COUNT; ++f) {
        if (claims(caps, f) && claims(accuracy_caps, f)) {
            len = record_text_char(dst, cap, len, ',');
            len =
                record_text_str(dst, cap, len, record_accuracy_word(field_of(sample, f)->accuracy));
        }
    }
    len = record_text_char(dst, cap, len, '\n');
    return record_text_overflowed(len, cap) ? 0u : len;
}
