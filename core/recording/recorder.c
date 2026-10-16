// SPDX-License-Identifier: Apache-2.0

#include "recording/recorder.h"

#include <string.h>

#include "recording/record_format.h"
#include "recording/record_limits.h"

#define LOG_TAG "recorder"

// --- the watch -------------------------------------------------------------------

void recorder_watch_init(recorder_watch_t *w)
{
    memset(w, 0, sizeof(*w));
}

size_t recorder_watch(recorder_watch_t *w, const sensor_manager_t *m, uint64_t now_ms,
                      uint64_t wall_ms, recorder_event_t *out, size_t cap)
{
    size_t n = 0u;
    for (size_t i = 0u; i < RECORDER_MAX_SOURCES && n < cap; ++i) {
        const sensor_manager_slot_t *slot = sensor_manager_slot(m, i);
        if (slot == NULL) {
            break;
        }
        const bool read = slot->reads != w->reads[i];
        const bool failed = slot->failures != w->failures[i];
        if (!read && !failed) {
            continue;
        }
        w->reads[i] = slot->reads;
        w->failures[i] = slot->failures;

        recorder_event_t *ev = &out[n++];
        ev->source_id = slot->source->id;
        ev->caps = slot->source->caps;
        // A read and a failure in one poll cannot both have happened (the
        // manager makes at most one measurement per slot per poll), and if
        // they did the sample holds the failure, which happened last.
        ev->failed = failed;
        ev->sample = slot->sample;
        ev->now_ms = now_ms;
        ev->wall_ms = wall_ms;
    }
    return n;
}

// --- the recorder ----------------------------------------------------------------

recorder_config_t recorder_config_default(void)
{
    const recorder_config_t cfg = {
        .flush_interval_ms = RECORD_FLUSH_INTERVAL_MS,
        .dir_cap_bytes = RECORD_DATA_DIR_CAP_BYTES,
        .retry_ms = RECORD_RETRY_MS,
    };
    return cfg;
}

void recorder_init(recorder_t *r, port_files_t *files, const port_log_t *log,
                   const time_zone_t *zone, const recorder_config_t *cfg)
{
    memset(r, 0, sizeof(*r));
    r->files = files;
    r->log = log;
    r->zone = zone;
    r->cfg = *cfg;
    r->state = RECORDER_WAITING;
    r->last_error = HAL_OK;
}

static recorder_source_t *source_by_id(recorder_t *r, uint8_t id)
{
    for (size_t i = 0u; i < r->count; ++i) {
        if (r->sources[i].id == id) {
            return &r->sources[i];
        }
    }
    return NULL;
}

hal_status_t recorder_add_source(recorder_t *r, uint8_t id, const char *tag,
                                 sensor_caps_t accuracy_caps)
{
    if (id == 0u || !record_tag_ok(tag) || source_by_id(r, id) != NULL) {
        return HAL_ERR_INVALID_ARG;
    }
    if (r->count >= RECORDER_MAX_SOURCES) {
        return HAL_ERR_NO_MEM;
    }
    recorder_source_t *src = &r->sources[r->count++];
    memset(src, 0, sizeof(*src));
    src->id = id;
    memcpy(src->tag, tag, strlen(tag) + 1u);
    src->accuracy_caps = accuracy_caps;
    return HAL_OK;
}

void recorder_set_undated_seq(recorder_t *r, uint32_t seq)
{
    r->undated_seq = seq;
}

const char *recorder_state_name(recorder_state_t state)
{
    switch (state) {
        case RECORDER_WAITING:
            return "waiting";
        case RECORDER_ON:
            return "on";
        case RECORDER_NO_MEDIUM:
            return "no_medium";
        case RECORDER_FULL:
            return "full";
        case RECORDER_FAILED:
            return "failed";
        default:
            return "?";
    }
}

static const char *status_name(hal_status_t status)
{
    switch (status) {
        case HAL_OK:
            return "ok";
        case HAL_ERR_TIMEOUT:
            return "timeout";
        case HAL_ERR_IO:
            return "io";
        case HAL_ERR_NOT_FOUND:
            return "not_found";
        case HAL_ERR_INVALID_ARG:
            return "invalid_arg";
        case HAL_ERR_NO_MEM:
            return "no_mem";
        case HAL_ERR_UNSUPPORTED:
            return "unsupported";
        case HAL_ERR_BUSY:
            return "busy";
        case HAL_ERR_NOT_READY:
            return "not_ready";
        default:
            return "?";
    }
}

// The state a failed call puts the recorder in.
static recorder_state_t state_for(hal_status_t status)
{
    switch (status) {
        case HAL_ERR_NOT_READY:
            return RECORDER_NO_MEDIUM;
        case HAL_ERR_NO_MEM:
            return RECORDER_FULL;
        default:
            return RECORDER_FAILED;
    }
}

// Logs a change of state once; staying in a state says nothing more.
static void set_state(recorder_t *r, recorder_state_t state, hal_status_t status, uint64_t now_ms)
{
    if (state == RECORDER_FULL || state == RECORDER_FAILED) {
        r->retry_at_ms = now_ms + r->cfg.retry_ms;
    }
    r->last_error = status;
    if (state == r->state) {
        return;
    }
    r->state = state;
    if (state == RECORDER_ON) {
        return;  // opening a file says where it records
    }
    const log_field_t fields[] = {
        log_str("state", recorder_state_name(state)),
        log_str("status", status_name(status)),
        log_u64("rows_dropped", recorder_rows_dropped(r)),
    };
    port_log_write(r->log, (state == RECORDER_NO_MEDIUM) ? LOG_LEVEL_INFO : LOG_LEVEL_WARN, LOG_TAG,
                   "recording stopped", fields, sizeof(fields) / sizeof(fields[0]));
}

static void drop_buffer(recorder_source_t *src)
{
    src->rows_dropped += src->buf_rows;
    src->buf_len = 0u;
    src->buf_rows = 0u;
}

// A call on the open file failed: the buffer is lost, the handle released --
// close() always releases it -- and a file nothing reached is removed, so a
// full card is not left littered with empty ones.
static void abandon_file(recorder_t *r, recorder_source_t *src, hal_status_t status,
                         uint64_t now_ms)
{
    drop_buffer(src);
    if (src->file != NULL) {
        (void)r->files->close(r->files, src->file);
        src->file = NULL;
        if (src->file_bytes == 0u && status != HAL_ERR_NOT_READY) {
            (void)r->files->remove(r->files, src->path);
        }
    }
    src->unflushed = false;
    set_state(r, state_for(status), status, now_ms);
}

// Appends the buffer and, with `and_flush`, flushes the file. False when the
// file was lost on the way.
static bool write_buffer(recorder_t *r, recorder_source_t *src, bool and_flush, uint64_t now_ms)
{
    if (src->file == NULL) {
        drop_buffer(src);
        return false;
    }
    if (src->buf_len > 0u) {
        const hal_status_t status = r->files->append(r->files, src->file, src->buf, src->buf_len);
        if (status != HAL_OK) {
            abandon_file(r, src, status, now_ms);
            return false;
        }
        src->rows_written += src->buf_rows;
        src->bytes_written += src->buf_len;
        src->file_bytes += src->buf_len;
        src->buf_len = 0u;
        src->buf_rows = 0u;
        src->unflushed = true;
    }
    if (and_flush && src->unflushed) {
        const hal_status_t status = r->files->flush(r->files, src->file);
        if (status != HAL_OK) {
            abandon_file(r, src, status, now_ms);
            return false;
        }
        src->unflushed = false;
    }
    return true;
}

static void close_file(recorder_t *r, recorder_source_t *src, uint64_t now_ms)
{
    if (src->file == NULL) {
        return;
    }
    if (!write_buffer(r, src, false, now_ms)) {
        return;  // abandoned, and so already closed
    }
    const hal_status_t status = r->files->close(r->files, src->file);
    src->file = NULL;
    src->unflushed = false;
    if (status != HAL_OK) {
        set_state(r, state_for(status), status, now_ms);
    }
}

// Keeps data/ within its bound before a new file of `key` starts, never
// touching the day of a file open now.
static hal_status_t enforce_cap(recorder_t *r, const char *key)
{
    const char *protected_keys[RECORDER_MAX_SOURCES + 1u];
    size_t n = 0u;
    protected_keys[n++] = key;
    for (size_t i = 0u; i < r->count; ++i) {
        if (r->sources[i].file != NULL) {
            protected_keys[n++] = r->sources[i].key;
        }
    }
    record_retention_t result;
    const hal_status_t status = record_enforce_cap(r->files, RECORD_DATA_DIR, r->cfg.dir_cap_bytes,
                                                   protected_keys, n, &result);
    if (status == HAL_OK && result.files_removed > 0u) {
        r->files_removed += result.files_removed;
        r->bytes_removed += result.bytes_removed;
        const log_field_t fields[] = {
            log_str("dir", RECORD_DATA_DIR),
            log_u64("files_removed", result.files_removed),
            log_u64("bytes_removed", result.bytes_removed),
            log_u64("total_bytes", result.total_bytes),
            log_bool("still_over", result.over_cap),
        };
        port_log_write(r->log, LOG_LEVEL_INFO, LOG_TAG, "oldest days removed", fields,
                       sizeof(fields) / sizeof(fields[0]));
    }
    return status;
}

static bool open_file(recorder_t *r, recorder_source_t *src, const char *key, sensor_caps_t caps,
                      uint64_t row_ms, uint64_t now_ms)
{
    if ((r->state == RECORDER_FULL || r->state == RECORDER_FAILED) && now_ms < r->retry_at_ms) {
        return false;
    }

    hal_status_t status = enforce_cap(r, key);
    if (status == HAL_ERR_NOT_READY) {
        set_state(r, RECORDER_NO_MEDIUM, status, now_ms);
        return false;
    }
    uint32_t index = 0u;
    status = record_next_index(r->files, RECORD_DATA_DIR, key, src->tag, RECORD_DATA_EXT, &index);
    if (status != HAL_OK) {
        set_state(r, state_for(status), status, now_ms);
        return false;
    }
    char path[PORT_FILES_PATH_MAX];
    if (!record_name_build(path, sizeof(path), RECORD_DATA_DIR, key, src->tag, index,
                           RECORD_DATA_EXT)) {
        set_state(r, RECORDER_FAILED, HAL_ERR_INVALID_ARG, now_ms);
        return false;
    }
    port_file_t *file = NULL;
    status = r->files->open_append(r->files, path, &file);
    if (status != HAL_OK) {
        set_state(r, state_for(status), status, now_ms);
        return false;
    }

    src->file = file;
    memcpy(src->path, path, sizeof(path));
    memcpy(src->key, key, sizeof(src->key));
    src->file_caps = caps;
    src->first_row_ms = row_ms;
    src->file_bytes = 0u;
    src->unflushed = false;
    src->files_opened++;

    // The preamble and the header lead the buffer, which is empty here: the
    // previous file's rows were appended when it was closed.
    size_t len = record_preamble(src->buf, sizeof(src->buf), src->tag, src->id);
    len += record_header(src->buf + len, sizeof(src->buf) - len, caps, src->accuracy_caps);
    src->buf_len = len;
    src->buf_rows = 0u;

    set_state(r, RECORDER_ON, HAL_OK, now_ms);
    const log_field_t fields[] = {
        log_str("path", path),
        log_u64("source_id", src->id),
        log_u64("files_opened", src->files_opened),
    };
    port_log_write(r->log, LOG_LEVEL_INFO, LOG_TAG, "recording", fields,
                   sizeof(fields) / sizeof(fields[0]));
    return true;
}

// The row's file: its local day, or this boot's undated key. False when the
// undated number cannot be learnt from the medium.
static bool key_for(recorder_t *r, uint64_t row_wall_ms, uint64_t now_ms, char *key)
{
    if (row_wall_ms != 0u) {
        const civil_time_t local = civil_time_local(row_wall_ms, r->zone);
        record_key_for_day(key, RECORD_KEY_MAX, &local);
        return true;
    }
    if (r->undated_seq == 0u) {
        uint32_t seq = 0u;
        const hal_status_t status = record_next_undated_seq(r->files, RECORD_DATA_DIR, &seq);
        if (status != HAL_OK) {
            set_state(r, state_for(status), status, now_ms);
            return false;
        }
        r->undated_seq = seq;
    }
    record_key_undated(key, RECORD_KEY_MAX, r->undated_seq);
    return true;
}

void recorder_record(recorder_t *r, const recorder_event_t *ev)
{
    recorder_source_t *src = source_by_id(r, ev->source_id);
    if (src == NULL) {
        return;
    }
    const uint64_t now_ms = ev->now_ms;
    if (!r->flush_clock_started) {
        r->last_flush_ms = now_ms;
        r->flush_clock_started = true;
    }

    // The row's own time: when its values were taken, which a slow poll
    // puts a little before the event's.
    uint64_t row_ms = record_sample_time_ms(&ev->sample, ev->caps);
    if (row_ms == 0u || row_ms > now_ms) {
        row_ms = now_ms;
    }
    const uint64_t age_ms = now_ms - row_ms;
    const uint64_t row_wall_ms = (ev->wall_ms > age_ms) ? ev->wall_ms - age_ms : 0u;

    char key[RECORD_KEY_MAX];
    if (!key_for(r, row_wall_ms, now_ms, key)) {
        src->rows_dropped++;
        return;
    }

    // A new day, the clock set, or a source claiming other fields: the next
    // file, and the old one's rows on the medium first.
    if (src->file != NULL && (strcmp(key, src->key) != 0 || ev->caps != src->file_caps)) {
        close_file(r, src, now_ms);
    }
    if (src->file == NULL && !open_file(r, src, key, ev->caps, row_ms, now_ms)) {
        src->rows_dropped++;
        return;
    }

    const uint64_t t_offset_ms = (row_ms >= src->first_row_ms) ? row_ms - src->first_row_ms : 0u;
    for (unsigned attempt = 0u; attempt < 2u; ++attempt) {
        const size_t n =
            record_row(src->buf + src->buf_len, sizeof(src->buf) - src->buf_len, &ev->sample,
                       ev->caps, src->accuracy_caps, t_offset_ms, row_wall_ms);
        if (n > 0u) {
            src->buf_len += n;
            src->buf_rows++;
            return;
        }
        // The buffer is full: on to the file, without the flush, and again.
        if (src->buf_len == 0u || !write_buffer(r, src, false, now_ms)) {
            break;
        }
    }
    src->rows_dropped++;
}

void recorder_flush(recorder_t *r, uint64_t now_ms)
{
    for (size_t i = 0u; i < r->count; ++i) {
        if (r->sources[i].file != NULL) {
            (void)write_buffer(r, &r->sources[i], true, now_ms);
        }
    }
    r->last_flush_ms = now_ms;
    r->flush_clock_started = true;
}

void recorder_tick(recorder_t *r, uint64_t now_ms)
{
    if (r->flush_clock_started && now_ms >= r->last_flush_ms &&
        now_ms - r->last_flush_ms >= r->cfg.flush_interval_ms) {
        recorder_flush(r, now_ms);
    }
}

void recorder_close(recorder_t *r, uint64_t now_ms)
{
    for (size_t i = 0u; i < r->count; ++i) {
        close_file(r, &r->sources[i], now_ms);
    }
    // Another medium may come next, and its undated files are its own.
    r->undated_seq = 0u;
}

uint32_t recorder_rows_written(const recorder_t *r)
{
    uint32_t n = 0u;
    for (size_t i = 0u; i < r->count; ++i) {
        n += r->sources[i].rows_written;
    }
    return n;
}

uint32_t recorder_rows_dropped(const recorder_t *r)
{
    uint32_t n = 0u;
    for (size_t i = 0u; i < r->count; ++i) {
        n += r->sources[i].rows_dropped;
    }
    return n;
}

uint64_t recorder_bytes_written(const recorder_t *r)
{
    uint64_t n = 0u;
    for (size_t i = 0u; i < r->count; ++i) {
        n += r->sources[i].bytes_written;
    }
    return n;
}
