// SPDX-License-Identifier: Apache-2.0
//
// The task that owns the microSD card (docs/adr/0024-sd-card-recordings-and-
// debug-log.md). Everything that waits on the card happens here and nowhere
// else: mounting it, the sensor recordings (core/recording/recorder.h), the
// debug log drained from log_tee's ring (core/recording/log_file.h), keeping
// both within their bounds. The event loop only posts what the recorder's
// watch found (card_writer_post(): a copy into a queue, never a wait), and
// the console asks for a mount, an eject or a status through
// card_writer_request(), which this task answers between its own work.
//
// No card is a normal state: an automatic mount is tried at start and every
// CARD_WRITER_MOUNT_RETRY_MS, quietly, and a change of state -- no card,
// mounted, lost, ejected -- is logged once. While no card is mounted the log
// stays in the ring, its oldest lines kept, for a card inserted later.

#ifndef HAC_APPS_FIRMWARE_ESP32_CARD_WRITER_H
#define HAC_APPS_FIRMWARE_ESP32_CARD_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "adp_sd.h"
#include "domain/civil_time.h"
#include "hal_status.h"
#include "port_clock.h"
#include "port_log.h"
#include "recording/log_file.h"
#include "recording/recorder.h"

#ifdef __cplusplus
extern "C" {
#endif

// CHOICE: how often an absent card is looked for again. Each attempt is the
// SD driver's initialisation timing out against an empty slot -- some
// hundreds of milliseconds of this task and the SPI bus, nothing logged. A
// minute finds a card inserted without sd_mount soon enough.
#define CARD_WRITER_MOUNT_RETRY_MS 60000u

// CHOICE: how long the debug log waits for the wall clock before lines
// without one go to an undated file. main.c sets the clock from the RTC
// about a second after boot; waiting lets the boot's lines carry their real
// time, worked back from the clock once it is set.
#define CARD_WRITER_LOG_CLOCK_GRACE_MS 15000u

// CHOICE: how often the task wakes with nothing posted, to drain the log and
// flush. A quarter second keeps the console's requests prompt.
#define CARD_WRITER_TICK_MS 250u

// CHOICE: the recorder events the queue holds -- one per source per poll,
// so this is minutes of sensor cycles if the card stalls.
#define CARD_WRITER_QUEUE_DEPTH 16u

// CHOICE: how often the task logs what it has written, so an unattended
// run's own log says how the card kept up.
#define CARD_WRITER_STATS_INTERVAL_MS (60u * 60u * 1000u)

typedef struct {
    const port_clock_t *clock;
    const port_log_t *log;
    adp_sd_t *sd;          // adp_sd_init()-ed
    recorder_t *recorder;  // recorder_init()-ed over adp_sd's port, sources added
    log_file_t *log_file;  // log_file_init()-ed over adp_sd's port
} card_writer_deps_t;

// Starts the task. `deps` is copied; what it points to is borrowed for the
// life of the firmware. False when the queue or the task cannot be made.
bool card_writer_start(const card_writer_deps_t *deps, uint32_t stack_bytes, int priority,
                       int core_id);

// From the event loop: hands the recorder's events to the task. Never waits;
// an event that does not fit in the queue is dropped and counted.
void card_writer_post(const recorder_event_t *events, size_t count);

typedef enum {
    CARD_WRITER_STATUS = 0,
    // The status and the card's free space, which the first time after a
    // mount may take FATFS seconds to count (adp_sd_card_info()).
    CARD_WRITER_SPACE,
    CARD_WRITER_MOUNT,  // mount now, with ESP-IDF's SD lines on the console
    CARD_WRITER_EJECT,  // flush, close, unmount: safe to remove; no automatic mount until MOUNT
} card_writer_cmd_t;

#define CARD_WRITER_DIRS 2u

typedef struct {
    hal_status_t result;  // of the command

    adp_sd_state_t sd_state;
    esp_err_t last_mount_err;
    uint32_t mount_attempts;
    uint32_t mounts;
    bool have_card;
    adp_sd_card_info_t card;
    struct {
        const char *dir;
        uint32_t files;
        uint64_t bytes;
    } dirs[CARD_WRITER_DIRS];

    recorder_state_t rec_state;
    hal_status_t rec_error;
    size_t source_count;
    struct {
        uint8_t id;
        char tag[RECORD_TAG_MAX];
        char path[PORT_FILES_PATH_MAX];  // "" while no file is open
        uint32_t rows_written;
        uint32_t rows_dropped;
        uint64_t bytes_written;
    } sources[RECORDER_MAX_SOURCES];
    uint32_t rec_files_removed;

    recorder_state_t log_state;
    char log_path[PORT_FILES_PATH_MAX];
    uint32_t log_lines_written;
    uint32_t log_lines_dropped;  // a missing or full card, and the ring overflowing
    uint64_t log_bytes_written;
    uint32_t ring_used;
    uint32_t ring_high_water;
    uint32_t ring_dropped_total;

    uint32_t events_posted;
    uint32_t events_dropped;
    uint32_t post_max_us;  // the longest card_writer_post() took the event loop
    uint32_t slowest_write_us;
    uint32_t slowest_flush_us;
    uint32_t stack_free_min_bytes;
} card_writer_status_t;

// From another task (the console): runs `cmd` on the card's task and fills
// `out`, which must stay valid until the answer comes -- static storage, as
// the console has. HAL_ERR_TIMEOUT when no answer comes within `timeout_ms`.
hal_status_t card_writer_request(card_writer_cmd_t cmd, card_writer_status_t *out,
                                 uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif  // HAC_APPS_FIRMWARE_ESP32_CARD_WRITER_H
