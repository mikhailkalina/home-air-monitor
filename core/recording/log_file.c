// SPDX-License-Identifier: Apache-2.0

#include "recording/log_file.h"

#include <string.h>

#include "recording/record_format.h"
#include "recording/record_limits.h"
#include "text/text_append.h"

// "YYYY-MM-DDTHH:MM:SS.mmmZ " before a line with a wall time.
#define PREFIX_MAX (RECORD_UTC_MS_LEN + 2u)

// A note this module writes itself: a prefix and a sentence.
#define NOTE_MAX 160u

void log_file_init(log_file_t *lf, port_files_t *files, const time_zone_t *zone)
{
    memset(lf, 0, sizeof(*lf));
    lf->files = files;
    lf->zone = zone;
    lf->flush_interval_ms = RECORD_LOG_FLUSH_INTERVAL_MS;
    lf->dir_cap_bytes = RECORD_LOG_DIR_CAP_BYTES;
    lf->retry_ms = RECORD_RETRY_MS;
    lf->state = RECORDER_WAITING;
    lf->last_error = HAL_OK;
}

void log_file_set_undated_seq(log_file_t *lf, uint32_t seq)
{
    lf->undated_seq = seq;
}

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

static void set_state(log_file_t *lf, hal_status_t status, uint64_t now_ms)
{
    lf->state = (status == HAL_OK) ? RECORDER_ON : state_for(status);
    lf->last_error = status;
    if (lf->state == RECORDER_FULL || lf->state == RECORDER_FAILED) {
        lf->retry_at_ms = now_ms + lf->retry_ms;
    }
}

static void lose(log_file_t *lf, uint32_t lines)
{
    lf->lines_dropped += lines;
    lf->lost_unreported += lines;
}

static void abandon(log_file_t *lf, hal_status_t status, uint64_t now_ms)
{
    lose(lf, lf->buf_lines);
    lf->buf_len = 0u;
    lf->buf_lines = 0u;
    if (lf->file != NULL) {
        (void)lf->files->close(lf->files, lf->file);
        lf->file = NULL;
    }
    lf->unflushed = false;
    set_state(lf, status, now_ms);
}

static bool write_buffer(log_file_t *lf, bool and_flush, uint64_t now_ms)
{
    if (lf->file == NULL) {
        lose(lf, lf->buf_lines);
        lf->buf_len = 0u;
        lf->buf_lines = 0u;
        return false;
    }
    if (lf->buf_len > 0u) {
        const hal_status_t status = lf->files->append(lf->files, lf->file, lf->buf, lf->buf_len);
        if (status != HAL_OK) {
            abandon(lf, status, now_ms);
            return false;
        }
        lf->lines_written += lf->buf_lines;
        lf->bytes_written += lf->buf_len;
        lf->buf_len = 0u;
        lf->buf_lines = 0u;
        lf->unflushed = true;
    }
    if (and_flush && lf->unflushed) {
        const hal_status_t status = lf->files->flush(lf->files, lf->file);
        if (status != HAL_OK) {
            abandon(lf, status, now_ms);
            return false;
        }
        lf->unflushed = false;
    }
    return true;
}

static void close_file(log_file_t *lf, uint64_t now_ms)
{
    if (lf->file == NULL || !write_buffer(lf, false, now_ms)) {
        return;
    }
    const hal_status_t status = lf->files->close(lf->files, lf->file);
    lf->file = NULL;
    lf->unflushed = false;
    if (status != HAL_OK) {
        set_state(lf, status, now_ms);
    }
}

static size_t prefix(char *dst, size_t cap, uint64_t wall_ms)
{
    if (wall_ms == 0u) {
        dst[0] = '\0';
        return 0u;
    }
    size_t len = record_format_utc(dst, cap, 0u, wall_ms, true);
    len = text_append_char(dst, cap, len, ' ');
    return text_overflowed(len, cap) ? 0u : len;
}

// Into the buffer, appending it first if the line does not fit.
static bool buffer_line(log_file_t *lf, uint64_t now_ms, const char *head, size_t head_len,
                        const char *text, size_t len)
{
    if (head_len + len > sizeof(lf->buf)) {
        return false;  // longer than any line the log makes
    }
    if (lf->buf_len + head_len + len > sizeof(lf->buf) && !write_buffer(lf, false, now_ms)) {
        return false;
    }
    memcpy(lf->buf + lf->buf_len, head, head_len);
    memcpy(lf->buf + lf->buf_len + head_len, text, len);
    lf->buf_len += head_len + len;
    lf->buf_lines++;
    return true;
}

// "<prefix>log: <n> lines lost <why>\n"
static size_t note(char *dst, size_t cap, uint64_t wall_ms, uint32_t lines, const char *why)
{
    size_t len = prefix(dst, cap, wall_ms);
    len = text_append_str(dst, cap, len, "log: ");
    len = text_append_u64(dst, cap, len, lines, 1u);
    len = text_append_str(dst, cap, len, (lines == 1u) ? " line lost " : " lines lost ");
    len = text_append_str(dst, cap, len, why);
    len = text_append_char(dst, cap, len, '\n');
    return text_overflowed(len, cap) ? 0u : len;
}

static bool open_file(log_file_t *lf, const char *key, uint64_t now_ms, uint64_t wall_ms)
{
    if ((lf->state == RECORDER_FULL || lf->state == RECORDER_FAILED) && now_ms < lf->retry_at_ms) {
        return false;
    }
    const char *protected_keys[] = {key};
    record_retention_t retention;
    hal_status_t status = record_enforce_cap(lf->files, RECORD_LOG_DIR, lf->dir_cap_bytes,
                                             protected_keys, 1u, &retention);
    if (status == HAL_ERR_NOT_READY) {
        set_state(lf, status, now_ms);
        return false;
    }
    lf->files_removed += retention.files_removed;

    char path[PORT_FILES_PATH_MAX];
    if (!record_name_build(path, sizeof(path), RECORD_LOG_DIR, key, NULL, 1u, RECORD_LOG_EXT)) {
        set_state(lf, HAL_ERR_INVALID_ARG, now_ms);
        return false;
    }
    port_file_t *file = NULL;
    status = lf->files->open_append(lf->files, path, &file);
    if (status != HAL_OK) {
        set_state(lf, status, now_ms);
        return false;
    }
    lf->file = file;
    memcpy(lf->path, path, sizeof(path));
    memcpy(lf->key, key, sizeof(lf->key));
    lf->unflushed = false;
    set_state(lf, HAL_OK, now_ms);

    if (lf->lost_unreported > 0u) {
        char text[NOTE_MAX];
        const size_t n = note(text, sizeof(text), wall_ms, lf->lost_unreported,
                              "while the card could not be written");
        if (n > 0u && buffer_line(lf, now_ms, "", 0u, text, n)) {
            lf->lost_unreported = 0u;
        }
    }
    return true;
}

static bool key_for(log_file_t *lf, uint64_t wall_ms, uint64_t now_ms, char *key)
{
    if (wall_ms != 0u) {
        const civil_time_t local = civil_time_local(wall_ms, lf->zone);
        record_key_for_day(key, RECORD_KEY_MAX, &local);
        return true;
    }
    if (lf->undated_seq == 0u) {
        uint32_t seq = 0u;
        const hal_status_t status = record_next_undated_seq(lf->files, RECORD_LOG_DIR, &seq);
        if (status != HAL_OK) {
            set_state(lf, status, now_ms);
            return false;
        }
        lf->undated_seq = seq;
    }
    record_key_undated(key, RECORD_KEY_MAX, lf->undated_seq);
    return true;
}

void log_file_write(log_file_t *lf, uint64_t now_ms, uint64_t line_wall_ms, const char *text,
                    size_t len)
{
    if (!lf->flush_clock_started) {
        lf->last_flush_ms = now_ms;
        lf->flush_clock_started = true;
    }
    char key[RECORD_KEY_MAX];
    if (!key_for(lf, line_wall_ms, now_ms, key)) {
        lose(lf, 1u);
        return;
    }
    if (lf->file != NULL && strcmp(key, lf->key) != 0) {
        close_file(lf, now_ms);
    }
    if (lf->file == NULL && !open_file(lf, key, now_ms, line_wall_ms)) {
        lose(lf, 1u);
        return;
    }
    char head[PREFIX_MAX];
    const size_t head_len = prefix(head, sizeof(head), line_wall_ms);
    if (!buffer_line(lf, now_ms, head, head_len, text, len)) {
        lose(lf, 1u);
    }
}

void log_file_note_lost(log_file_t *lf, uint64_t now_ms, uint64_t wall_ms, uint32_t lines,
                        const char *why)
{
    if (lines == 0u) {
        return;
    }
    lf->lines_dropped += lines;
    char text[NOTE_MAX];
    char reason[NOTE_MAX];
    size_t len = text_append_str(reason, sizeof(reason), 0u, "before this one: ");
    len = text_append_str(reason, sizeof(reason), len, why);
    if (text_overflowed(len, sizeof(reason))) {
        return;
    }
    const size_t n = note(text, sizeof(text), 0u, lines, reason);
    if (n > 0u) {
        log_file_write(lf, now_ms, wall_ms, text, n);
    }
}

void log_file_flush(log_file_t *lf, uint64_t now_ms)
{
    if (lf->file != NULL) {
        (void)write_buffer(lf, true, now_ms);
    }
    lf->last_flush_ms = now_ms;
    lf->flush_clock_started = true;
}

void log_file_tick(log_file_t *lf, uint64_t now_ms)
{
    if (lf->flush_clock_started && now_ms >= lf->last_flush_ms &&
        now_ms - lf->last_flush_ms >= lf->flush_interval_ms) {
        log_file_flush(lf, now_ms);
    }
}

void log_file_close(log_file_t *lf, uint64_t now_ms)
{
    close_file(lf, now_ms);
    lf->undated_seq = 0u;
}
