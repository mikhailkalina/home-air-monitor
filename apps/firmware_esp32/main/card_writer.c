// SPDX-License-Identifier: Apache-2.0

#include "card_writer.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "board_button.h"
#include "fw_update.h"
#include "log_tee.h"
#include "recording/record_files.h"
#include "recording/record_limits.h"
#include "usb_disk.h"
#include "usb_disk_limits.h"
#include "usb_disk_pure.h"

#define TAG "card_writer"

#define BYTES_PER_MB (1024u * 1024u)

// The most log lines written per wake, so a backlog -- the boot's, once a
// card is there -- does not keep the posted events and the console waiting.
#define LOG_LINES_PER_WAKE 64u

#define REQUEST_QUEUE_DEPTH 2u

// CHOICE: from an installed update's last log line to the restart, so the
// console's terminal shows it too.
#define UPDATE_RESTART_DELAY_MS 200u

// The most passes over the log's ring before an update's restart, each
// LOG_LINES_PER_WAKE lines: the whole ring, 1024 lines, and then some.
#define UPDATE_LOG_DRAIN_PASSES 32u

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

    // The USB disk (docs/adr/0036): lent, the PHY's switch due at lend_at_ms.
    bool lend_pending;
    uint64_t lend_at_ms;
    usb_disk_button_t button;

    // Written by the event loop's task only; read here for the status.
    volatile uint32_t events_posted;
    volatile uint32_t events_dropped;
    volatile uint32_t post_max_us;

    // Written here only, read by the event loop's task for the diagnostics
    // screen (card_writer_recorder_state()): one aligned word each.
    volatile uint32_t published_rec_state;
    volatile uint32_t running;
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

static void drain_log(uint64_t now, bool all);
static void restart_into_update(void);

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

    // Every mount -- at boot, sd_mount, the minute's, after a USB disk
    // session -- looks for firmware in the card's root (docs/adr/0037).
    if (fw_update_from_card(ADP_SD_MOUNT_POINT) == FW_UPDATE_RESULT_INSTALLED) {
        restart_into_update();
    }
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
        case ADP_SD_LENT:
            msg = "the card is lent to USB: recording and the log file are off until it is back";
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
// set it get theirs too. Without a card the lines stay in the ring. `all`:
// now, without waiting for the clock -- the restart into an update.
static void drain_log(uint64_t now, bool all)
{
    log_ring_t *ring = log_tee_ring();
    if (ring == NULL || !adp_sd_mounted(s_w.deps.sd)) {
        return;
    }
    const uint64_t wall = s_w.deps.clock->wall_ms(s_w.deps.clock);
    if (!all && wall == 0u && now < CARD_WRITER_LOG_CLOCK_GRACE_MS &&
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

// --- the card as a USB disk (docs/adr/0036) -------------------------------------------

static bool lent_or_pending(void)
{
    return s_w.lend_pending || usb_disk_active();
}

// The card made ready for a host: the log's lines onto it first, every file
// closed, the filesystem unmounted, the card brought up for its sectors. The
// PHY's switch comes USB_DISK_SWITCH_DELAY_MS later, so that the console's
// answer still reaches the terminal.
static hal_status_t prepare_lend(uint64_t now, const char *by)
{
    if (lent_or_pending()) {
        return HAL_ERR_BUSY;
    }
    drain_log(now, false);
    recorder_close(s_w.deps.recorder, now);
    log_file_close(s_w.deps.log_file, now);
    const hal_status_t status = adp_sd_lend(s_w.deps.sd);
    if (status != HAL_OK) {
        const log_field_t fields[] = {
            log_str("by", by),
            log_i64("status", (int64_t)status),
            log_i64("esp_err", (int64_t)s_w.deps.sd->last_mount_err),
        };
        port_log_write(s_w.deps.log, LOG_LEVEL_WARN, TAG, "the card could not be lent to USB",
                       fields, sizeof(fields) / sizeof(fields[0]));
        s_w.next_mount_ms = now;  // the attempt unmounted it: mount it again at once
        return status;
    }
    uint32_t sectors = 0u;
    uint32_t sector_bytes = 0u;
    (void)adp_sd_lent_geometry(s_w.deps.sd, &sectors, &sector_bytes);
    const log_field_t fields[] = {
        log_str("by", by),
        log_u64("capacity_mb", (uint64_t)sectors * sector_bytes / BYTES_PER_MB),
        log_u64("switch_in_ms", USB_DISK_SWITCH_DELAY_MS),
    };
    port_log_write(s_w.deps.log, LOG_LEVEL_INFO, TAG,
                   "lending the card to USB: the serial port goes, a disk comes; eject the disk "
                   "or press the button to end it",
                   fields, sizeof(fields) / sizeof(fields[0]));
    s_w.lend_pending = true;
    s_w.lend_at_ms = now + USB_DISK_SWITCH_DELAY_MS;
    return HAL_OK;
}

// The card back from the host, and mounted again -- loudly, since the host
// may have left it unreadable.
static void take_back(uint64_t now, const char *why)
{
    adp_sd_end_lend(s_w.deps.sd);
    s_w.auto_mount = true;
    usb_disk_stats_t st;
    usb_disk_stats(&st);
    const log_field_t fields[] = {
        log_str("why", why),
        log_u64("lasted_ms", st.lasted_ms),
        log_u64("configurations", st.configurations),
        log_u64("read_kb", st.bytes_read / 1024u),
        log_u64("written_kb", st.bytes_written / 1024u),
        log_u64("io_errors", st.io_errors),
        log_u64("refused", st.refused),
        log_u64("slowest_read_us", st.slowest_read_us),
        log_u64("slowest_write_us", st.slowest_write_us),
    };
    port_log_write(s_w.deps.log, LOG_LEVEL_INFO, TAG, "the card is back from USB", fields,
                   sizeof(fields) / sizeof(fields[0]));
    (void)try_mount(false, now);
    note_state(now);
}

// Each pass: the PHY's switch when it is due, the button, and the session's
// verdict while lent.
static void usb_step(uint64_t now)
{
    const usb_disk_button_event_t button =
        usb_disk_button_sample(&s_w.button, board_button_pressed(), now);

    if (s_w.lend_pending && now >= s_w.lend_at_ms) {
        s_w.lend_pending = false;
        const hal_status_t status = usb_disk_start(s_w.deps.sd, now);
        const log_field_t fields[] = {log_i64("status", (int64_t)status)};
        port_log_write(s_w.deps.log, status == HAL_OK ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR, TAG,
                       status == HAL_OK ? "usb disk started: the PHY is the USB-OTG controller's"
                                        : "usb disk not started: USB-Serial-JTAG kept the PHY",
                       fields, 1u);
        if (status != HAL_OK) {
            take_back(now, "not_started");
            return;
        }
    }

    if (usb_disk_active()) {
        if (button == USB_DISK_BUTTON_PRESS) {
            usb_disk_button_consume(&s_w.button);
            usb_disk_press();
        }
        const usb_disk_verdict_t verdict = usb_disk_verdict(now);
        if (verdict != USB_DISK_KEEP) {
            usb_disk_stop(now);
            take_back(now, usb_disk_verdict_name(verdict));
        }
        return;
    }
    if (button == USB_DISK_BUTTON_HOLD && !s_w.lend_pending) {
        usb_disk_button_consume(&s_w.button);
        (void)prepare_lend(now, "button");
    }
}

// --- firmware from the card (docs/adr/0037) -------------------------------------------

// The update installed: its lines onto the card, every file closed, the card
// unmounted cleanly, then the restart into the new image.
static void restart_into_update(void)
{
    const uint64_t now = now_ms();
    log_ring_t *ring = log_tee_ring();
    for (unsigned i = 0u; i < UPDATE_LOG_DRAIN_PASSES && ring != NULL && log_ring_used(ring) > 0u;
         ++i) {
        drain_log(now, true);
    }
    recorder_close(s_w.deps.recorder, now);
    log_file_close(s_w.deps.log_file, now);
    adp_sd_unmount(s_w.deps.sd, ADP_SD_EJECTED);
    vTaskDelay(pdMS_TO_TICKS(UPDATE_RESTART_DELAY_MS));
    esp_restart();
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
    usb_disk_stats(&out->usb);
}

static void handle_request(const request_t *req, uint64_t now)
{
    hal_status_t result = HAL_OK;
    switch (req->cmd) {
        case CARD_WRITER_MOUNT:
            if (lent_or_pending()) {
                result = HAL_ERR_BUSY;
                break;
            }
            s_w.auto_mount = true;
            // Not quiet: whoever typed sd_mount wants to see why it failed.
            result = try_mount(false, now);
            break;
        case CARD_WRITER_EJECT:
            if (lent_or_pending()) {
                result = HAL_ERR_BUSY;
                break;
            }
            recorder_close(s_w.deps.recorder, now);
            log_file_close(s_w.deps.log_file, now);
            adp_sd_unmount(s_w.deps.sd, ADP_SD_EJECTED);
            s_w.auto_mount = false;
            break;
        case CARD_WRITER_LEND:
            result = prepare_lend(now, "console");
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

// How long the queue is waited on: not at all while lent (TinyUSB waited),
// until the PHY's switch while one is due, a tick otherwise.
static TickType_t wait_ticks(uint64_t now)
{
    if (usb_disk_active()) {
        return 0;
    }
    uint64_t ms = CARD_WRITER_TICK_MS;
    if (s_w.lend_pending) {
        const uint64_t due = (s_w.lend_at_ms > now) ? s_w.lend_at_ms - now : 0u;
        ms = (due < ms) ? due : ms;
    }
    return pdMS_TO_TICKS(ms);
}

static void writer_task(void *arg)
{
    (void)arg;
    uint64_t now = now_ms();
    (void)try_mount(true, now);
    note_state(now);
    s_w.next_stats_ms = now + CARD_WRITER_STATS_INTERVAL_MS;
    s_w.published_rec_state = (uint32_t)s_w.deps.recorder->state;
    s_w.running = 1u;

    for (;;) {
        // Lent, TinyUSB's wait takes this task's: the host's reads and
        // writes run inside it, and the queue is looked at between them.
        if (usb_disk_active()) {
            usb_disk_step(USB_DISK_STEP_MS, now_ms());
        }
        recorder_event_t ev;
        if (xQueueReceive(s_w.events, &ev, wait_ticks(now_ms())) == pdTRUE) {
            do {
                recorder_record(s_w.deps.recorder, &ev);
            } while (xQueueReceive(s_w.events, &ev, 0) == pdTRUE);
        }
        now = now_ms();

        request_t req;
        while (xQueueReceive(s_w.requests, &req, 0) == pdTRUE) {
            handle_request(&req, now);
        }
        usb_step(now);
        if (!adp_sd_mounted(s_w.deps.sd) && !lent_or_pending() && s_w.auto_mount &&
            now >= s_w.next_mount_ms) {
            (void)try_mount(true, now);
        }
        note_state(now);
        drain_log(now, false);
        recorder_tick(s_w.deps.recorder, now);
        s_w.published_rec_state = (uint32_t)s_w.deps.recorder->state;
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
    usb_disk_button_init(&s_w.button, USB_DISK_BUTTON_DEBOUNCE_MS, USB_DISK_BUTTON_HOLD_MS);
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

bool card_writer_recorder_state(recorder_state_t *out)
{
    if (s_w.running == 0u) {
        return false;
    }
    *out = (recorder_state_t)s_w.published_rec_state;
    return true;
}
