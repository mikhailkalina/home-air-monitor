// SPDX-License-Identifier: Apache-2.0
//
// The sensor recordings (docs/adr/0024-sd-card-recordings-and-debug-log.md):
// one file per source, a row whenever the source has a new sample or a new
// failure, in the scenario format the simulator replays
// (core/recording/record_format.h), rotated at local midnight.
//
// Two halves, because on the board they run on two tasks:
//
//   * the WATCH (recorder_watch()) runs where the sensor manager does -- the
//     event loop -- after each poll. It compares each slot's counters of
//     reads and failures with what it saw last and turns every change into a
//     recorder_event_t: a copy of the slot's sample, the time and the wall
//     time. It touches no medium and takes microseconds.
//
//   * the RECORDER (recorder_record(), recorder_tick()) runs wherever the
//     medium may be waited on: on the board the task that owns the card,
//     which receives the events through a queue; in the simulator, the
//     same loop. It formats a row, picks the file -- the row's local day, or
//     an undated file while the wall clock is not set -- buffers it, and
//     appends and flushes the buffer through port_files every
//     RECORD_FLUSH_INTERVAL_MS and at every rotation.
//
// A file is one continuous episode on one monotonic clock: t_offset_s counts
// from its first row, so a file is never reopened once closed -- a reboot, a
// failed write or the clock set back starts the next file of that day
// ("_2", "_3"; record_files.h). A missing medium is a state, not a fault:
// rows are dropped and counted, the state is logged once, and the next row
// tries again. A full or failing one is retried after RECORD_RETRY_MS.
//
// Pure, like the sensor manager: no allocation, no stdio, no clock of its
// own -- every time comes in with an event or a tick.

#ifndef HAC_CORE_RECORDING_RECORDER_H
#define HAC_CORE_RECORDING_RECORDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/sensor_manager.h"
#include "domain/civil_time.h"
#include "hal_status.h"
#include "port_files.h"
#include "port_log.h"
#include "port_sensor.h"
#include "recording/record_files.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RECORDER_MAX_SOURCES SENSOR_MANAGER_MAX_SOURCES

// CHOICE: rows a source buffers between flushes. A row is under 100 bytes,
// a flush interval holds four at BSEC's pace; a fuller buffer is appended
// early, without the flush.
#define RECORDER_BUFFER_BYTES 2048u

// --- the watch -------------------------------------------------------------------

// One change in one source: a new sample, or a new failure (`failed`), with
// every field the source claims as the sensor manager holds it now.
typedef struct {
    uint8_t source_id;
    sensor_caps_t caps;
    bool failed;
    sensor_sample_t sample;
    uint64_t now_ms;   // the clock after the poll
    uint64_t wall_ms;  // the wall clock at now_ms; 0 while it is not set
} recorder_event_t;

typedef struct {
    uint32_t reads[RECORDER_MAX_SOURCES];
    uint32_t failures[RECORDER_MAX_SOURCES];
} recorder_watch_t;

void recorder_watch_init(recorder_watch_t *w);

// Writes up to `cap` events for the slots whose reads or failures moved since
// the last call, and returns how many. A slot that did not fit is reported
// at the next call, with its sample as it is then.
size_t recorder_watch(recorder_watch_t *w, const sensor_manager_t *m, uint64_t now_ms,
                      uint64_t wall_ms, recorder_event_t *out, size_t cap);

// --- the recorder ----------------------------------------------------------------

typedef enum {
    RECORDER_WAITING = 0,  // nothing to record yet
    RECORDER_ON,           // writing
    RECORDER_NO_MEDIUM,    // no medium: rows are dropped until one is there
    RECORDER_FULL,         // the medium is full: rows are dropped, retried
    RECORDER_FAILED,       // the medium refused otherwise: rows are dropped, retried
} recorder_state_t;

typedef struct {
    uint32_t flush_interval_ms;
    uint64_t dir_cap_bytes;
    uint32_t retry_ms;
} recorder_config_t;

typedef struct {
    uint8_t id;
    char tag[RECORD_TAG_MAX];
    sensor_caps_t accuracy_caps;  // the fields given a "_acc" column

    port_file_t *file;  // NULL while no file is open
    char path[PORT_FILES_PATH_MAX];
    char key[RECORD_KEY_MAX];
    sensor_caps_t file_caps;  // what the open file's header claims
    uint64_t first_row_ms;    // the open file's first row, on the monotonic clock
    uint64_t file_bytes;      // appended to the open file
    bool unflushed;           // appended since the last flush

    char buf[RECORDER_BUFFER_BYTES];
    size_t buf_len;
    uint32_t buf_rows;

    // Since recorder_init().
    uint32_t rows_written;  // appended to a file
    uint32_t rows_dropped;  // lost to a missing, full or failing medium
    uint64_t bytes_written;
    uint32_t files_opened;
} recorder_source_t;

typedef struct {
    port_files_t *files;
    const port_log_t *log;  // may be NULL
    const time_zone_t *zone;
    recorder_config_t cfg;

    recorder_source_t sources[RECORDER_MAX_SOURCES];
    size_t count;

    recorder_state_t state;
    hal_status_t last_error;  // what put it in its state, HAL_OK while on
    uint64_t retry_at_ms;     // full or failed: no file is opened before this
    uint64_t last_flush_ms;
    bool flush_clock_started;
    uint32_t undated_seq;  // 0 until an undated file is needed

    uint32_t files_removed;  // by the bound, since recorder_init()
    uint64_t bytes_removed;
} recorder_t;

// RECORD_FLUSH_INTERVAL_MS, RECORD_DATA_DIR_CAP_BYTES, RECORD_RETRY_MS.
recorder_config_t recorder_config_default(void);

// `files`, `zone` and `cfg` are required and kept by pointer or copied; the
// pointers must outlive the recorder. `log` may be NULL.
void recorder_init(recorder_t *r, port_files_t *files, const port_log_t *log,
                   const time_zone_t *zone, const recorder_config_t *cfg);

// Records the source `id` under `tag` (record_tag_ok()), with an accuracy
// column for the fields in `accuracy_caps`. Events from sources never added
// are ignored. HAL_ERR_INVALID_ARG for an id of 0, a duplicate id or a bad
// tag; HAL_ERR_NO_MEM past RECORDER_MAX_SOURCES.
hal_status_t recorder_add_source(recorder_t *r, uint8_t id, const char *tag,
                                 sensor_caps_t accuracy_caps);

// Which undated boot this is (record_next_undated_seq()), when the caller
// knows it; otherwise the recorder asks the medium itself when it first
// needs an undated file.
void recorder_set_undated_seq(recorder_t *r, uint32_t seq);

// One row for `ev`, into the right file's buffer.
void recorder_record(recorder_t *r, const recorder_event_t *ev);

// Appends and flushes every buffer once RECORD_FLUSH_INTERVAL_MS has passed
// since the last flush. Call it as often as convenient; `now_ms` is the
// monotonic clock.
void recorder_tick(recorder_t *r, uint64_t now_ms);

// Appends and flushes every buffer now.
void recorder_flush(recorder_t *r, uint64_t now_ms);

// Flushes and closes every file -- before the medium is taken away, and at
// exit. The next row opens a new file.
void recorder_close(recorder_t *r, uint64_t now_ms);

// Rows and bytes written, rows dropped, over every source.
uint32_t recorder_rows_written(const recorder_t *r);
uint32_t recorder_rows_dropped(const recorder_t *r);
uint64_t recorder_bytes_written(const recorder_t *r);

// "waiting", "on", "no_medium", "full", "failed"; never NULL.
const char *recorder_state_name(recorder_state_t state);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_RECORDING_RECORDER_H
