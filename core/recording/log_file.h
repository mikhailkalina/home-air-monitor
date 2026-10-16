// SPDX-License-Identifier: Apache-2.0
//
// The debug log's files (docs/adr/0024-sd-card-recordings-and-debug-log.md):
// lines of text -- what ESP_LOG printed, on the board -- appended to
// log/<YYYY-MM-DD>.log for the local day of each line, or to this boot's
// log/undated-NNNN.log while the wall clock is not set, each line prefixed
// with its UTC time to the millisecond when there is one. Buffered, and
// flushed every RECORD_LOG_FLUSH_INTERVAL_MS and at every change of day; log/
// kept within RECORD_LOG_DIR_CAP_BYTES, the oldest day removed first, as the
// recordings are (record_files.h).
//
// Unlike a recording, a day's log file is appended to across boots: a line
// carries its own time, and the boot's first line says it is one.
//
// Pure over port_files, so the host tests it. It logs nothing itself -- on
// the board its caller is the task that drains the log, and a line about the
// log written into the log is a loop -- but it says, in the file, how many
// lines a missing or full card cost once it can write again.

#ifndef HAC_CORE_RECORDING_LOG_FILE_H
#define HAC_CORE_RECORDING_LOG_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "domain/civil_time.h"
#include "port_files.h"
#include "recording/record_files.h"
#include "recording/recorder.h"

#ifdef __cplusplus
extern "C" {
#endif

// CHOICE: what is appended at once. A flush interval at a busy log's pace
// (a redraw decision, a flush timing, a sensor line every few seconds),
// with room for a burst like the boot's.
#define LOG_FILE_BUFFER_BYTES 4096u

typedef struct {
    port_files_t *files;
    const time_zone_t *zone;
    uint32_t flush_interval_ms;
    uint64_t dir_cap_bytes;
    uint32_t retry_ms;

    port_file_t *file;
    char path[PORT_FILES_PATH_MAX];
    char key[RECORD_KEY_MAX];
    bool unflushed;

    char buf[LOG_FILE_BUFFER_BYTES];
    size_t buf_len;
    uint32_t buf_lines;

    recorder_state_t state;  // the recordings' states, with the same meaning
    hal_status_t last_error;
    uint64_t retry_at_ms;
    uint64_t last_flush_ms;
    bool flush_clock_started;
    uint32_t undated_seq;  // 0 until an undated file is needed

    uint32_t lost_unreported;  // lines lost since the last note in the file

    // Since log_file_init().
    uint32_t lines_written;
    uint32_t lines_dropped;
    uint64_t bytes_written;
    uint32_t files_removed;
} log_file_t;

void log_file_init(log_file_t *lf, port_files_t *files, const time_zone_t *zone);

// Which undated boot this is, as for recorder_set_undated_seq().
void log_file_set_undated_seq(log_file_t *lf, uint32_t seq);

// One line, `text` of `len` bytes ending in '\n', logged at `line_wall_ms`
// (0: no wall clock). `now_ms` is the monotonic clock, for the flush and the
// retry.
void log_file_write(log_file_t *lf, uint64_t now_ms, uint64_t line_wall_ms, const char *text,
                    size_t len);

// Says in the file that `lines` lines were lost before the next one, and
// why (a phrase: "the log's buffer in RAM was full"), and counts them.
void log_file_note_lost(log_file_t *lf, uint64_t now_ms, uint64_t wall_ms, uint32_t lines,
                        const char *why);

void log_file_tick(log_file_t *lf, uint64_t now_ms);
void log_file_flush(log_file_t *lf, uint64_t now_ms);
void log_file_close(log_file_t *lf, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_RECORDING_LOG_FILE_H
