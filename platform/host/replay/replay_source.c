// SPDX-License-Identifier: Apache-2.0

#include "replay_source.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- small helpers -----------------------------------------------------------

typedef struct {
    const char *p;
    size_t len;
} token_t;

#define REPLAY_MAX_COLUMNS 16u
#define REPLAY_COL_KIND_STATUS (-1)

static void trim(const char **p, size_t *len)
{
    while (*len > 0 && isspace((unsigned char)(*p)[0])) {
        (*p)++;
        (*len)--;
    }
    while (*len > 0 && isspace((unsigned char)(*p)[*len - 1])) {
        (*len)--;
    }
}

static bool token_eq(token_t t, const char *lit)
{
    size_t n = strlen(lit);
    return t.len == n && memcmp(t.p, lit, n) == 0;
}

// Splits one line on ','. Tokens beyond `max_tokens` are still counted (the
// returned count can exceed max_tokens) so the caller can tell "too many
// columns" apart from a genuine parse.
static size_t split_columns(const char *line, size_t line_len, token_t *tokens, size_t max_tokens)
{
    size_t count = 0;
    const char *p = line;
    const char *end = line + line_len;

    while (true) {
        const char *comma = memchr(p, ',', (size_t)(end - p));
        const char *field_end = comma ? comma : end;

        if (count < max_tokens) {
            tokens[count].p = p;
            tokens[count].len = (size_t)(field_end - p);
            trim(&tokens[count].p, &tokens[count].len);
        }
        count++;

        if (!comma) {
            break;
        }
        p = comma + 1;
    }
    return count;
}

static bool parse_double_full(token_t tok, double *out)
{
    char buf[64];

    if (tok.len == 0 || tok.len >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, tok.p, tok.len);
    buf[tok.len] = '\0';

    char *endptr = NULL;
    double v = strtod(buf, &endptr);
    if (endptr != buf + tok.len) {
        return false;  // trailing garbage, or nothing parsed at all
    }
    *out = v;
    return true;
}

static bool map_field_name(token_t tok, replay_field_t *out)
{
    static const struct {
        const char *name;
        replay_field_t field;
    } NAMES[] = {
        {"temperature_c", REPLAY_FIELD_TEMPERATURE_C},
        {"humidity_rh", REPLAY_FIELD_HUMIDITY_RH},
        {"pressure_hpa", REPLAY_FIELD_PRESSURE_HPA},
        {"co2_ppm", REPLAY_FIELD_CO2_PPM},
        {"iaq", REPLAY_FIELD_IAQ},
        {"voc_index", REPLAY_FIELD_VOC_INDEX},
        {"gas_res_ohm", REPLAY_FIELD_GAS_RESISTANCE_OHM},
    };

    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (token_eq(tok, NAMES[i].name)) {
            *out = NAMES[i].field;
            return true;
        }
    }
    return false;
}

static bool map_status_word(token_t tok, replay_row_status_t *out)
{
    static const struct {
        const char *word;
        replay_row_status_t status;
    } WORDS[] = {
        {"ok", REPLAY_ROW_OK},       {"warmup", REPLAY_ROW_WARMUP},
        {"stale", REPLAY_ROW_STALE}, {"error", REPLAY_ROW_ERROR},
        {"nan", REPLAY_ROW_NAN},     {"out_of_range", REPLAY_ROW_OUT_OF_RANGE},
    };

    for (size_t i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); i++) {
        if (token_eq(tok, WORDS[i].word)) {
            *out = WORDS[i].status;
            return true;
        }
    }
    return false;
}

static hal_status_t timeline_push_row(replay_timeline_t *tl, const replay_row_t *row)
{
    if (tl->row_count == tl->row_capacity) {
        size_t new_cap = (tl->row_capacity == 0) ? 64u : tl->row_capacity * 2u;
        if (new_cap > REPLAY_TIMELINE_MAX_ROWS) {
            new_cap = REPLAY_TIMELINE_MAX_ROWS;
        }
        if (new_cap <= tl->row_capacity) {
            return HAL_ERR_NO_MEM;  // already at the cap
        }
        replay_row_t *grown = realloc(tl->rows, new_cap * sizeof(*grown));
        if (!grown) {
            return HAL_ERR_NO_MEM;
        }
        tl->rows = grown;
        tl->row_capacity = new_cap;
    }
    tl->rows[tl->row_count++] = *row;
    return HAL_OK;
}

// --- the parser --------------------------------------------------------------

hal_status_t replay_parse_buffer(const char *data, size_t len, replay_timeline_t *out)
{
    memset(out, 0, sizeof(*out));

    int col_kind[REPLAY_MAX_COLUMNS];  // index 0 (t_offset_s) is implicit and unused here
    size_t header_col_count = 0;
    bool have_header = false;
    hal_status_t status = HAL_OK;

    const char *cursor = data;
    const char *end = data + len;

    while (cursor < end) {
        const char *nl = memchr(cursor, '\n', (size_t)(end - cursor));
        const char *line_end = nl ? nl : end;
        const char *line = cursor;
        size_t line_len = (size_t)(line_end - cursor);
        cursor = nl ? nl + 1 : end;

        if (line_len > 0 && line[line_len - 1] == '\r') {
            line_len--;
        }

        const char *trimmed = line;
        size_t trimmed_len = line_len;
        trim(&trimmed, &trimmed_len);
        if (trimmed_len == 0 || trimmed[0] == '#') {
            continue;  // blank line or whole-line comment
        }

        token_t tokens[REPLAY_MAX_COLUMNS];
        size_t count = split_columns(trimmed, trimmed_len, tokens, REPLAY_MAX_COLUMNS);
        if (count > REPLAY_MAX_COLUMNS) {
            status = HAL_ERR_INVALID_ARG;  // absurd number of columns
            goto fail;
        }

        if (!have_header) {
            if (count == 0 || !token_eq(tokens[0], "t_offset_s")) {
                status = HAL_ERR_INVALID_ARG;  // the mandatory first column is missing
                goto fail;
            }
            for (size_t i = 1; i < count; i++) {
                replay_field_t field;
                if (token_eq(tokens[i], "status")) {
                    col_kind[i] = REPLAY_COL_KIND_STATUS;
                } else if (map_field_name(tokens[i], &field)) {
                    if (out->field_present[field]) {
                        status = HAL_ERR_INVALID_ARG;  // duplicate column
                        goto fail;
                    }
                    out->field_present[field] = true;
                    col_kind[i] = (int)field;
                } else {
                    status = HAL_ERR_INVALID_ARG;  // unrecognized column name
                    goto fail;
                }
            }
            header_col_count = count;
            have_header = true;
            continue;
        }

        if (count != header_col_count) {
            status = HAL_ERR_INVALID_ARG;  // row does not match the header's shape
            goto fail;
        }

        replay_row_t row;
        memset(&row, 0, sizeof(row));

        if (!parse_double_full(tokens[0], &row.t_offset_s)) {
            status = HAL_ERR_INVALID_ARG;  // malformed t_offset_s
            goto fail;
        }

        // Pass 1: resolve the row's status, since it decides whether the
        // numeric columns are allowed to be blank in pass 2.
        row.status = REPLAY_ROW_OK;
        for (size_t i = 1; i < count; i++) {
            if (col_kind[i] == REPLAY_COL_KIND_STATUS) {
                if (tokens[i].len > 0 && !map_status_word(tokens[i], &row.status)) {
                    status = HAL_ERR_INVALID_ARG;  // unrecognized status word
                    goto fail;
                }
                break;
            }
        }

        // Pass 2: numeric columns. A blank cell is only meaningful when the
        // row is "ok"; on a fault row, the number is never used, so a
        // scenario author may leave it out.
        for (size_t i = 1; i < count; i++) {
            if (col_kind[i] == REPLAY_COL_KIND_STATUS) {
                continue;
            }
            replay_field_t field = (replay_field_t)col_kind[i];
            if (tokens[i].len == 0) {
                if (row.status == REPLAY_ROW_OK) {
                    status = HAL_ERR_INVALID_ARG;  // a live field with no value
                    goto fail;
                }
                continue;
            }
            if (!parse_double_full(tokens[i], &row.value[field])) {
                status = HAL_ERR_INVALID_ARG;  // malformed number
                goto fail;
            }
        }

        status = timeline_push_row(out, &row);
        if (status != HAL_OK) {
            goto fail;
        }
    }

    if (!have_header || out->row_count == 0) {
        status = HAL_ERR_INVALID_ARG;  // no header, or a header with no data rows
        goto fail;
    }

    out->loop_period_s = (out->row_count > 1) ? out->rows[out->row_count - 1].t_offset_s : 0.0;
    return HAL_OK;

fail:
    replay_timeline_dispose(out);
    return status;
}

hal_status_t replay_parse_file(const char *path, replay_timeline_t *out)
{
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) {
        return HAL_ERR_NOT_FOUND;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return HAL_ERR_IO;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return HAL_ERR_IO;
    }

    char *buf = malloc((size_t)size > 0 ? (size_t)size : 1u);
    if (!buf) {
        fclose(f);
        return HAL_ERR_NO_MEM;
    }

    size_t read_n = (size > 0) ? fread(buf, 1, (size_t)size, f) : 0u;
    fclose(f);
    if (read_n != (size_t)size) {
        free(buf);
        return HAL_ERR_IO;
    }

    hal_status_t status = replay_parse_buffer(buf, read_n, out);
    free(buf);
    return status;
}

void replay_timeline_dispose(replay_timeline_t *timeline)
{
    free(timeline->rows);
    memset(timeline, 0, sizeof(*timeline));
}

// --- playback ------------------------------------------------------------

// Finds the two rows bracketing `phase` (virtual seconds within
// [0, loop_period_s)) and the interpolation fraction between them. With one
// row, or a degenerate (zero-length) timeline, both indices collapse to 0.
static void find_bracket(const replay_timeline_t *tl, double phase, size_t *i0, size_t *i1,
                         double *frac)
{
    if (tl->row_count <= 1 || tl->loop_period_s <= 0.0) {
        *i0 = 0;
        *i1 = 0;
        *frac = 0.0;
        return;
    }

    size_t idx = tl->row_count - 2;  // falls back to the last segment
    for (size_t i = 0; i + 1 < tl->row_count; i++) {
        if (phase >= tl->rows[i].t_offset_s && phase < tl->rows[i + 1].t_offset_s) {
            idx = i;
            break;
        }
    }

    *i0 = idx;
    *i1 = idx + 1;
    double seg = tl->rows[*i1].t_offset_s - tl->rows[*i0].t_offset_s;
    *frac = (seg > 0.0) ? (phase - tl->rows[*i0].t_offset_s) / seg : 0.0;
}

static void sample_field(const replay_timeline_t *tl, replay_field_t field, size_t i0, size_t i1,
                         double frac, bool interpolate, uint64_t ts_ms, uint8_t id,
                         measurement_f32_t *out)
{
    if (!tl->field_present[field]) {
        *out = measurement_unavailable();
        return;
    }

    const replay_row_t *r0 = &tl->rows[i0];
    switch (r0->status) {
        case REPLAY_ROW_OK:
            break;
        case REPLAY_ROW_WARMUP:
            *out = measurement_status(VAL_WARMUP, ts_ms, id);
            return;
        case REPLAY_ROW_STALE:
            *out = measurement_status(VAL_STALE, ts_ms, id);
            return;
        case REPLAY_ROW_NAN:
            *out =
                measurement_ok((float)NAN, ts_ms, id);  // measurement_ok() rejects it as VAL_ERROR
            return;
        case REPLAY_ROW_ERROR:
        case REPLAY_ROW_OUT_OF_RANGE:
        default:
            *out = measurement_status(VAL_ERROR, ts_ms, id);
            return;
    }

    // r0 is "ok". Interpolate towards r1 only when it is "ok" too: a step
    // towards a fault value would misrepresent the fault as a gradual change.
    double value = r0->value[field];
    if (interpolate && i0 != i1 && tl->rows[i1].status == REPLAY_ROW_OK) {
        value += frac * (tl->rows[i1].value[field] - value);
    }
    *out = measurement_ok((float)value, ts_ms, id);
}

static replay_source_t *self_of(sensor_source_t *port)
{
    return (replay_source_t *)port->impl;
}

static hal_status_t rs_init(sensor_source_t *self)
{
    (void)self;
    return HAL_OK;  // the file is already loaded by replay_source_init()
}

static hal_status_t rs_start(sensor_source_t *self)
{
    replay_source_t *rs = self_of(self);

    rs->start_ms = rs->clock->now_ms(rs->clock);
    rs->started = true;
    return HAL_OK;
}

static uint32_t rs_ready_in_ms(const sensor_source_t *self)
{
    (void)self;
    return 0u;  // replayed data has no warm-up; it is ready as soon as it starts
}

static hal_status_t rs_read(sensor_source_t *self, sensor_sample_t *out)
{
    replay_source_t *rs = self_of(self);

    if (!rs->started) {
        return HAL_ERR_NOT_READY;
    }

    uint64_t now = rs->clock->now_ms(rs->clock);
    uint64_t elapsed_ms = (now >= rs->start_ms) ? (now - rs->start_ms) : 0u;
    double elapsed_s = (double)elapsed_ms / 1000.0;

    double phase = 0.0;
    if (rs->timeline.loop_period_s > 0.0) {
        phase = fmod(elapsed_s, rs->timeline.loop_period_s);
    }

    size_t i0, i1;
    double frac;
    find_bracket(&rs->timeline, phase, &i0, &i1, &frac);

    uint8_t id = self->id;
    sample_field(&rs->timeline, REPLAY_FIELD_TEMPERATURE_C, i0, i1, frac, rs->interpolate, now, id,
                 &out->temperature_c);
    sample_field(&rs->timeline, REPLAY_FIELD_HUMIDITY_RH, i0, i1, frac, rs->interpolate, now, id,
                 &out->humidity_rh);
    sample_field(&rs->timeline, REPLAY_FIELD_PRESSURE_HPA, i0, i1, frac, rs->interpolate, now, id,
                 &out->pressure_hpa);
    sample_field(&rs->timeline, REPLAY_FIELD_CO2_PPM, i0, i1, frac, rs->interpolate, now, id,
                 &out->co2_ppm);
    sample_field(&rs->timeline, REPLAY_FIELD_IAQ, i0, i1, frac, rs->interpolate, now, id,
                 &out->iaq);
    sample_field(&rs->timeline, REPLAY_FIELD_VOC_INDEX, i0, i1, frac, rs->interpolate, now, id,
                 &out->voc_index);
    sample_field(&rs->timeline, REPLAY_FIELD_GAS_RESISTANCE_OHM, i0, i1, frac, rs->interpolate, now,
                 id, &out->gas_resistance_ohm);

    return HAL_OK;
}

static hal_status_t rs_suspend(sensor_source_t *self)
{
    (void)self;
    return HAL_OK;  // nothing to flush: there is no hardware behind a replay source
}

static void rs_deinit(sensor_source_t *self)
{
    replay_timeline_dispose(&self_of(self)->timeline);
}

static void set_name_from_path(char *dst, size_t dst_size, const char *path)
{
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    size_t n = strlen(base);
    if (n >= dst_size) {
        n = dst_size - 1;
    }
    memcpy(dst, base, n);
    dst[n] = '\0';
}

hal_status_t replay_source_init(replay_source_t *out, const char *path, sensor_caps_t caps,
                                uint8_t id, const port_clock_t *clock, bool interpolate)
{
    memset(out, 0, sizeof(*out));

    hal_status_t status = replay_parse_file(path, &out->timeline);
    if (status != HAL_OK) {
        return status;
    }

    out->clock = clock;
    out->interpolate = interpolate;
    set_name_from_path(out->name, sizeof(out->name), path);

    out->port.name = out->name;
    out->port.id = id;
    out->port.caps = caps;
    out->port.init = rs_init;
    out->port.start = rs_start;
    out->port.ready_in_ms = rs_ready_in_ms;
    out->port.read = rs_read;
    out->port.suspend = rs_suspend;
    out->port.deinit = rs_deinit;
    out->port.impl = out;

    return HAL_OK;
}

void replay_source_deinit(replay_source_t *rs)
{
    replay_timeline_dispose(&rs->timeline);
}
