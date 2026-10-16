// SPDX-License-Identifier: Apache-2.0

#include "card_writer.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "log_tee.h"
#include "recording/record_files.h"
#include "recording/record_limits.h"

#define TAG "card_writer"

#define BYTES_PER_MB (1024u * 1024u)

// The most log lines written per wake, so a backlog -- the boot's, once a
// card is there -- does not keep the posted events and the console waiting.
#define LOG_LINES_PER_WAKE 64u

#define REQUEST_QUEUE_DEPTH 2u

typedef struct {
    card_writer_cmd_t cmd;
    card_writer_status_t *out;
} request_t;

typedef struct {
    card_writer_deps_t deps;
    QueueHandle_t events;
    QueueHandle_t requests;
    SemaphoreHandle_t done;

    adp_sd_state_t logged_state;  // what note_state() last said
    bool logged_once;
    uint32_t mounted_generation;  // the mount on_mounted() last saw
    bool auto_mount;              // false from an eject until a mount is asked for
    uint64_t next_mount_ms;
    uint64_t next_stats_ms;
    uint32_t stack_free_min;

    // Written by the event loop's task only; read here for the status.
    volatile uint32_t events_posted;
    volatile uint32_t events_dropped;
    volatile uint32_t post_max_us;
} writer_t;

static writer_t s_w;

static uint64_t now_ms(void)
{
    return s_w.deps.clock->now_ms(s_w.deps.clock);
}

static port_files_t *files(void)
{
    return adp_sd_port(s_w.deps.sd);
}

// --- the card --------------------------------------------------------------------

// A boot that needs undated files numbers them once, past every undated file
// in either directory (record_files.h).
static void choose_undated_seq(void)
{
    uint32_t data = 0u;
    uint32_t log = 0u;
    (void)record_next_undated_seq(files(), RECORD_DATA_DIR, &data);
    (void)record_next_undated_seq(files(), RECORD_LOG_DIR, &log);
    const uint32_t seq = (data > log) ? data : log;
    recorder_set_undated_seq(s_w.deps.recorder, seq);
    log_file_set_undated_seq(s_w.deps.log_file, seq);
}

static void on_mounted(void)
{
    choose_undated_seq();
    // Not its free space: counting it can mean reading the whole FAT, seconds
    // of the card working at boot. `sd` asks for it.
    adp_sd_card_info_t info;
    (void)adp_sd_card_info(s_w.deps.sd, false, &info);
    // What the card costs, measured where it is paid: the bus, FATFS and the
    // card's structures sit in internal RAM, the files' buffers in PSRAM.
    const log_field_t fields[] = {
        log_str("card", info.name),
        log_u64("capacity_mb", info.capacity_bytes / BYTES_PER_MB),
        log_u64("spi_khz", info.freq_khz),
        log_u64("free_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        log_u64("free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
    };
    port_log_write(s_w.deps.log, LOG_LEVEL_INFO, TAG, "card mounted", fields,
                   sizeof(fields) / sizeof(fields[0]));
    s_w.logged_state = ADP_SD_MOUNTED;
    s_w.logged_once = true;
    s_w.mounted_generation = s_w.deps.sd->generation;
}

static hal_status_t try_mount(bool quiet, uint64_t now)
{
    const hal_status_t status = adp_sd_mount(s_w.deps.sd, quiet);
    // A new mount, even one that followed a loss within the same wake.
    if (status == HAL_OK && s_w.deps.sd->generation != s_w.mounted_generation) {
        on_mounted();
    }
    s_w.next_mount_ms = now + CARD_WRITER_MOUNT_RETRY_MS;
    return status;
}

// Each change of the card's state, once. A card found gone closes the files
// the recorder and the log held on it.
static void note_state(uint64_t now)
{
    const adp_sd_t *sd = s_w.deps.sd;
    if (s_w.logged_once && sd->state == s_w.logged_state) {
        return;
    }
    if (sd->state == ADP_SD_LOST) {
        recorder_close(s_w.deps.recorder, now);
        log_file_close(s_w.deps.log_file, now);
    }
    const log_field_t fields[] = {
        log_str("state", adp_sd_state_name(sd->state)),
        log_i64("esp_err", (int64_t)sd->last_mount_err),
        log_u64("retry_ms", CARD_WRITER_MOUNT_RETRY_MS),
    };
    const char *msg = NULL;
    log_level_t level = LOG_LEVEL_INFO;
    switch (sd->state) {
        case ADP_SD_NO_CARD:
            msg = "no card: recording and the log file are off until one is mounted";
            break;
        case ADP_SD_UNREADABLE:
            msg = "the card holds no FAT filesystem this mounts: not formatted, recording off";
            level = LOG_LEVEL_WARN;
            break;
        case ADP_SD_LOST:
            msg = "the card stopped answering: unmounted, recording off";
            level = LOG_LEVEL_WARN;
            break;
        case ADP_SD_EJECTED:
            msg = "card ejected: safe to remove; sd_mount to use it again";
            break;
        case ADP_SD_MOUNTED:  // on_mounted() says it
        case ADP_SD_NOT_TRIED:
        default:
            break;
    }
    if (msg != NULL) {
        port_log_write(s_w.deps.log, level, TAG, msg, fields, sizeof(fields) / sizeof(fields[0]));
    }
    s_w.logged_state = sd->state;
    s_w.logged_once = true;
}

// --- the log -------------------------------------------------------------------------

// Lines from the ring to the card, each at the wall time it was logged --
// worked back from the clock now, so the boot's lines logged before the RTC
// set it get theirs too. Without a card the lines stay in the ring.
static void drain_log(uint64_t now)
{
    log_ring_t *ring = log_tee_ring();
    if (ring == NULL || !adp_sd_mounted(s_w.deps.sd)) {
        return;
    }
    const uint64_t wall = s_w.deps.clock->wall_ms(s_w.deps.clock);
    if (wall == 0u && now < CARD_WRITER_LOG_CLOCK_GRACE_MS &&
        log_ring_used(ring) < ring->slot_count / 2u) {
        return;
    }
    for (unsigned i = 0u; i < LOG_LINES_PER_WAKE; ++i) {
        const log_ring_slot_t *slot = log_ring_peek(ring, now);
        if (slot == NULL) {
            break;
        }
        const uint64_t age = (now > slot->mono_ms) ? now - slot->mono_ms : 0u;
        const uint64_t line_wall = (wall > age) ? wall - age : 0u;
        log_file_write(s_w.deps.log_file, now, line_wall, slot->text, slot->len);
        log_ring_release(ring);
    }
    log_file_note_lost(s_w.deps.log_file, now, wall, log_tee_take_dropped(),
                       "the log's buffer in RAM was full");
    log_file_note_lost(s_w.deps.log_file, now, wall, log_tee_take_held(),
                       "held off PSRAM while the panel's rail came up");
}

// --- the console's requests ----------------------------------------------------------

typedef struct {
    uint32_t files;
    uint64_t bytes;
} dir_count_t;

static void count_file(void *ctx, const char *name, uint64_t size_bytes)
{
    (void)name;
    dir_count_t *c = (dir_count_t *)ctx;
    c->files++;
    c->bytes += size_bytes;
}

static void fill_status(card_writer_status_t *out, bool with_free)
{
    memset(out, 0, sizeof(*out));
    const adp_sd_t *sd = s_w.deps.sd;
    out->sd_state = sd->state;
    out->last_mount_err = sd->last_mount_err;
    out->mount_attempts = sd->mount_attempts;
    out->mounts = sd->mounts;
    out->have_card = adp_sd_card_info(s_w.deps.sd, with_free, &out->card) == HAL_OK;
    static const char *const DIRS[CARD_WRITER_DIRS] = {RECORD_DATA_DIR, RECORD_LOG_DIR};
    for (size_t i = 0u; i < CARD_WRITER_DIRS; ++i) {
        dir_count_t count = {0};
        if (out->have_card) {
            (void)files()->list(files(), DIRS[i], count_file, &count);
        }
        out->dirs[i].dir = DIRS[i];
        out->dirs[i].files = count.files;
        out->dirs[i].bytes = count.bytes;
    }

    const recorder_t *rec = s_w.deps.recorder;
    out->rec_state = rec->state;
    out->rec_error = rec->last_error;
    out->rec_files_removed = rec->files_removed;
    out->source_count = rec->count;
    for (size_t i = 0u; i < rec->count; ++i) {
        const recorder_source_t *src = &rec->sources[i];
        out->sources[i].id = src->id;
        memcpy(out->sources[i].tag, src->tag, sizeof(out->sources[i].tag));
        if (src->file != NULL) {
            memcpy(out->sources[i].path, src->path, sizeof(out->sources[i].path));
        }
        out->sources[i].rows_written = src->rows_written;
        out->sources[i].rows_dropped = src->rows_dropped;
        out->sources[i].bytes_written = src->bytes_written;
    }

    const log_file_t *lf = s_w.deps.log_file;
    out->log_state = lf->state;
    if (lf->file != NULL) {
        memcpy(out->log_path, lf->path, sizeof(out->log_path));
    }
    out->log_lines_written = lf->lines_written;
    out->log_lines_dropped = lf->lines_dropped;
    out->log_bytes_written = lf->bytes_written;
    const log_ring_t *ring = log_tee_ring();
    if (ring != NULL) {
        out->ring_used = log_ring_used(ring);
        out->ring_high_water = ring->high_water;
        out->ring_dropped_total = ring->dropped_total;
    }

    out->events_posted = s_w.events_posted;
    out->events_dropped = s_w.events_dropped;
    out->post_max_us = s_w.post_max_us;
    out->slowest_write_us = sd->slowest_write_us;
    out->slowest_flush_us = sd->slowest_flush_us;
    out->stack_free_min_bytes = s_w.stack_free_min;
}

static void handle_request(const request_t *req, uint64_t now)
{
    hal_status_t result = HAL_OK;
    switch (req->cmd) {
        case CARD_WRITER_MOUNT:
            s_w.auto_mount = true;
            // Not quiet: whoever typed sd_mount wants to see why it failed.
            result = try_mount(false, now);
            break;
        case CARD_WRITER_EJECT:
            recorder_close(s_w.deps.recorder, now);
            log_file_close(s_w.deps.log_file, now);
            adp_sd_unmount(s_w.deps.sd, ADP_SD_EJECTED);
            s_w.auto_mount = false;
            break;
        case CARD_WRITER_STATUS:
        case CARD_WRITER_SPACE:
        default:
            break;
    }
    note_state(now);
    fill_status(req->out, req->cmd == CARD_WRITER_SPACE);
    req->out->result = result;
    (void)xSemaphoreGive(s_w.done);
}

// --- the task -----------------------------------------------------------------------

static void log_stats(void)
{
    const recorder_t *rec = s_w.deps.recorder;
    const log_file_t *lf = s_w.deps.log_file;
    const log_field_t fields[] = {
        log_u64("rows", recorder_rows_written(rec)),
        log_u64("rows_dropped", recorder_rows_dropped(rec)),
        log_u64("log_lines", lf->lines_written),
        log_u64("log_lines_dropped", lf->lines_dropped),
        log_u64("slowest_flush_us", s_w.deps.sd->slowest_flush_us),
        log_u64("post_max_us", s_w.post_max_us),
    };
    port_log_write(s_w.deps.log, LOG_LEVEL_INFO, TAG, "recording stats", fields,
                   sizeof(fields) / sizeof(fields[0]));
}

static void track_stack(void)
{
    // StackType_t is uint8_t on ESP-IDF's Xtensa port: bytes (event_loop.c).
    const uint32_t free_bytes = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    if (free_bytes < s_w.stack_free_min) {
        s_w.stack_free_min = free_bytes;
        const log_field_t fields[] = {log_u64("stack_free_min_bytes", free_bytes)};
        port_log_write(s_w.deps.log, LOG_LEVEL_INFO, TAG, "stack low-water mark", fields, 1u);
    }
}

static void writer_task(void *arg)
{
    (void)arg;
    uint64_t now = now_ms();
    (void)try_mount(true, now);
    note_state(now);
    s_w.next_stats_ms = now + CARD_WRITER_STATS_INTERVAL_MS;

    for (;;) {
        recorder_event_t ev;
        if (xQueueReceive(s_w.events, &ev, pdMS_TO_TICKS(CARD_WRITER_TICK_MS)) == pdTRUE) {
            do {
                recorder_record(s_w.deps.recorder, &ev);
            } while (xQueueReceive(s_w.events, &ev, 0) == pdTRUE);
        }
        now = now_ms();

        request_t req;
        while (xQueueReceive(s_w.requests, &req, 0) == pdTRUE) {
            handle_request(&req, now);
        }
        if (!adp_sd_mounted(s_w.deps.sd) && s_w.auto_mount && now >= s_w.next_mount_ms) {
            (void)try_mount(true, now);
        }
        note_state(now);
        drain_log(now);
        recorder_tick(s_w.deps.recorder, now);
        log_file_tick(s_w.deps.log_file, now);
        if (now >= s_w.next_stats_ms) {
            log_stats();
            s_w.next_stats_ms = now + CARD_WRITER_STATS_INTERVAL_MS;
        }
        track_stack();
    }
}

bool card_writer_start(const card_writer_deps_t *deps, uint32_t stack_bytes, int priority,
                       int core_id)
{
    memset(&s_w, 0, sizeof(s_w));
    s_w.deps = *deps;
    s_w.auto_mount = true;
    s_w.stack_free_min = UINT32_MAX;
    // The events are copies of sensor samples, ~200 bytes each: in PSRAM.
    s_w.events =
        xQueueCreateWithCaps(CARD_WRITER_QUEUE_DEPTH, sizeof(recorder_event_t), MALLOC_CAP_SPIRAM);
    s_w.requests = xQueueCreate(REQUEST_QUEUE_DEPTH, sizeof(request_t));
    s_w.done = xSemaphoreCreateBinary();
    if (s_w.events == NULL || s_w.requests == NULL || s_w.done == NULL) {
        port_log_write(deps->log, LOG_LEVEL_ERROR, TAG, "queues not created: no card writer", NULL,
                       0u);
        return false;
    }
    // Stack bytes, not words, on ESP-IDF (event_loop.c).
    if (xTaskCreatePinnedToCore(writer_task, "hac_card", stack_bytes, NULL, (UBaseType_t)priority,
                                NULL, (BaseType_t)core_id) != pdPASS) {
        port_log_write(deps->log, LOG_LEVEL_ERROR, TAG, "task not created: no card writer", NULL,
                       0u);
        return false;
    }
    return true;
}

void card_writer_post(const recorder_event_t *events, size_t count)
{
    if (s_w.events == NULL) {
        return;
    }
    for (size_t i = 0u; i < count; ++i) {
        const int64_t start = esp_timer_get_time();
        const bool sent = xQueueSend(s_w.events, &events[i], 0) == pdTRUE;
        const int64_t took = esp_timer_get_time() - start;
        if (sent) {
            s_w.events_posted++;
        } else {
            s_w.events_dropped++;
        }
        if (took > 0 && (uint64_t)took > s_w.post_max_us) {
            s_w.post_max_us = (uint32_t)took;
        }
    }
}

hal_status_t card_writer_request(card_writer_cmd_t cmd, card_writer_status_t *out,
                                 uint32_t timeout_ms)
{
    if (s_w.requests == NULL) {
        return HAL_ERR_NOT_READY;
    }
    (void)xSemaphoreTake(s_w.done, 0);  // an answer that came after a timeout
    const request_t req = {.cmd = cmd, .out = out};
    if (xQueueSend(s_w.requests, &req, pdMS_TO_TICKS(timeout_ms)) != pdTRUE ||
        xSemaphoreTake(s_w.done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return HAL_ERR_TIMEOUT;
    }
    return HAL_OK;
}
