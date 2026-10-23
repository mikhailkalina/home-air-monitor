// SPDX-License-Identifier: Apache-2.0

#include "fw_update.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sdkconfig.h"

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_assert.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The bootloader rolls back an image that never confirms itself only when it
// was built with rollback; and a new default in sdkconfig.defaults does not
// reach an sdkconfig made before it (CLAUDE.md). Without it an update that
// does not start would leave the board on it.
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "fw_update needs CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE (sdkconfig.defaults) -- delete \
this build's sdkconfig, or run idf.py reconfigure, so the default reaches it; then flash the \
new bootloader too"
#endif

// fw_image.h reads the image by these offsets; they are ESP-IDF's.
ESP_STATIC_ASSERT(sizeof(esp_image_header_t) == FW_IMAGE_HEADER_BYTES, "image header");
ESP_STATIC_ASSERT(offsetof(esp_image_header_t, chip_id) == FW_IMAGE_CHIP_ID_OFFSET, "chip id");
ESP_STATIC_ASSERT(sizeof(esp_image_segment_header_t) == FW_IMAGE_SEGMENT_HEADER_BYTES, "segment");
ESP_STATIC_ASSERT(ESP_IMAGE_HEADER_MAGIC == FW_IMAGE_MAGIC, "image magic");
ESP_STATIC_ASSERT(ESP_APP_DESC_MAGIC_WORD == FW_APP_DESC_MAGIC, "app description magic");
ESP_STATIC_ASSERT(sizeof(esp_app_desc_t) == FW_APP_DESC_BYTES, "app description");
ESP_STATIC_ASSERT(offsetof(esp_app_desc_t, version) == FW_APP_DESC_VERSION_OFFSET, "version");
ESP_STATIC_ASSERT(offsetof(esp_app_desc_t, project_name) == FW_APP_DESC_PROJECT_OFFSET, "name");
ESP_STATIC_ASSERT(offsetof(esp_app_desc_t, time) == FW_APP_DESC_TIME_OFFSET, "time");
ESP_STATIC_ASSERT(offsetof(esp_app_desc_t, date) == FW_APP_DESC_DATE_OFFSET, "date");
ESP_STATIC_ASSERT(offsetof(esp_app_desc_t, idf_ver) == FW_APP_DESC_IDF_VER_OFFSET, "idf_ver");

#define TAG "fw_update"

// CHOICE: how much of the file is read and written at a time: a flash
// sector, in internal RAM for the duration of the update only.
#define CHUNK_BYTES 4096u

// CHOICE: the card's task yields a tick after this much, so the idle tasks
// -- and the task watchdog that watches them -- run during an update of
// seconds; the SD driver and the flash writes block in between anyway.
#define YIELD_EVERY_BYTES (64u * 1024u)

#define PATH_MAX_BYTES 64u

static const port_log_t *s_log;
static fw_update_attempt_t s_last;
static bool s_attempted;

const char *fw_update_result_name(fw_update_result_t result)
{
    switch (result) {
        case FW_UPDATE_RESULT_NONE:
            return "none";
        case FW_UPDATE_RESULT_INSTALLED:
            return "installed";
        case FW_UPDATE_RESULT_REFUSED:
            return "refused";
        case FW_UPDATE_RESULT_FAILED:
            return "failed";
        default:
            return "?";
    }
}

static const char *state_name(const esp_partition_t *part)
{
    esp_ota_img_states_t state;
    if (part == NULL || esp_ota_get_state_partition(part, &state) != ESP_OK) {
        // No otadata entry for it: flashed by esptool with ota_data_initial,
        // booted as if it were the factory image.
        return "flashed";
    }
    switch (state) {
        case ESP_OTA_IMG_NEW:
            return "new";
        case ESP_OTA_IMG_PENDING_VERIFY:
            return "pending_verify";
        case ESP_OTA_IMG_VALID:
            return "valid";
        case ESP_OTA_IMG_INVALID:
            return "invalid";
        case ESP_OTA_IMG_ABORTED:
            return "aborted";
        case ESP_OTA_IMG_UNDEFINED:
        default:
            return "undefined";
    }
}

static uint32_t elapsed_ms(int64_t since_us)
{
    const int64_t us = esp_timer_get_time() - since_us;
    return (us > 0) ? (uint32_t)(us / 1000) : 0u;
}

void fw_update_boot(const port_log_t *log)
{
    s_log = log;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *app = esp_app_get_description();
    const log_field_t fields[] = {
        log_str("partition", running != NULL ? running->label : "?"),
        log_str("state", state_name(running)),
        log_str("version", app->version),
        log_str("project", app->project_name),
        log_str("built", app->date),
    };
    port_log_write(log, LOG_LEVEL_INFO, TAG, "running", fields, sizeof(fields) / sizeof(fields[0]));

    // An image the bootloader gave up on -- an update that reset before it
    // confirmed itself -- stays marked in the other slot until the next one.
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    if (invalid != NULL) {
        esp_app_desc_t desc;
        const bool known = esp_ota_get_partition_description(invalid, &desc) == ESP_OK;
        const log_field_t rb[] = {
            log_str("partition", invalid->label),
            log_str("version", known ? desc.version : "?"),
            log_str("state", state_name(invalid)),
        };
        port_log_write(log, LOG_LEVEL_WARN, TAG,
                       "the other slot holds an update that did not start: rolled back", rb,
                       sizeof(rb) / sizeof(rb[0]));
    }
}

void fw_update_confirm_running(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running == NULL || esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    const log_field_t fields[] = {
        log_str("partition", running->label),
        log_str("version", esp_app_get_description()->version),
        log_i64("esp_err", (int64_t)err),
    };
    port_log_write(s_log, err == ESP_OK ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR, TAG,
                   err == ESP_OK ? "the update confirmed: it reached its first frame"
                                 : "the update could not be confirmed: a reset will roll it back",
                   fields, sizeof(fields) / sizeof(fields[0]));
}

bool fw_update_last_attempt(fw_update_attempt_t *out)
{
    if (!s_attempted) {
        return false;
    }
    *out = s_last;
    return true;
}

// The file renamed, so the next mount does not try it again.
static void reject(const char *path)
{
    char rejected[PATH_MAX_BYTES + sizeof(FW_UPDATE_REJECTED_SUFFIX)];
    (void)snprintf(rejected, sizeof(rejected), "%s%s", path, FW_UPDATE_REJECTED_SUFFIX);
    (void)unlink(rejected);  // an earlier one, if any
    if (rename(path, rejected) != 0) {
        // Not renamed: deleted, rather than refused again at every mount.
        (void)unlink(path);
    }
}

static void log_attempt(log_level_t level, const char *msg)
{
    const fw_update_attempt_t *a = &s_last;
    const log_field_t fields[] = {
        log_str("result", fw_update_result_name(a->result)),
        log_str("verdict", fw_update_verdict_name(a->verdict)),
        log_str("step", a->step),
        log_i64("err", (int64_t)a->err),
        log_str("project", a->image.project),
        log_str("version", a->image.version),
        log_str("from_version", esp_app_get_description()->version),
        log_str("built", a->image.date),
        log_u64("bytes", a->file_bytes),
        log_str("slot", a->slot),
        log_u64("took_ms", a->took_ms),
    };
    port_log_write(s_log, level, TAG, msg, fields, sizeof(fields) / sizeof(fields[0]));
}

static fw_update_result_t finish(fw_update_result_t result, const char *step, esp_err_t err,
                                 int64_t start_us)
{
    s_last.result = result;
    s_last.step = step;
    s_last.err = (int32_t)err;
    s_last.took_ms = elapsed_ms(start_us);
    switch (result) {
        case FW_UPDATE_RESULT_INSTALLED:
            log_attempt(LOG_LEVEL_INFO, "firmware from the card installed: restarting into it");
            break;
        case FW_UPDATE_RESULT_REFUSED:
            log_attempt(LOG_LEVEL_WARN, "firmware on the card refused: renamed .rejected");
            break;
        case FW_UPDATE_RESULT_FAILED:
            log_attempt(LOG_LEVEL_ERROR, "firmware on the card not installed: the file stays");
            break;
        case FW_UPDATE_RESULT_NONE:
        default:
            break;
    }
    return result;
}

fw_update_result_t fw_update_from_card(const char *mount_point)
{
    char path[PATH_MAX_BYTES];
    (void)snprintf(path, sizeof(path), "%s/%s", mount_point, FW_UPDATE_FILE_NAME);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return FW_UPDATE_RESULT_NONE;
    }

    const int64_t start = esp_timer_get_time();
    memset(&s_last, 0, sizeof(s_last));
    s_attempted = true;
    s_last.step = "";
    s_last.file_bytes = (uint64_t)st.st_size;
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (slot != NULL) {
        (void)snprintf(s_last.slot, sizeof(s_last.slot), "%s", slot->label);
    }

    uint8_t *buf = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    FILE *f = fopen(path, "rb");
    if (buf == NULL || f == NULL || slot == NULL) {
        if (f != NULL) {
            (void)fclose(f);
        }
        heap_caps_free(buf);
        return finish(FW_UPDATE_RESULT_FAILED, buf == NULL ? "memory" : "read", (esp_err_t)errno,
                      start);
    }

    size_t n = fread(buf, 1u, CHUNK_BYTES, f);
    const fw_image_status_t parsed = fw_image_parse(buf, n, &s_last.image);
    s_last.verdict = fw_update_judge(parsed, &s_last.image, s_last.file_bytes,
                                     (uint16_t)CONFIG_IDF_FIRMWARE_CHIP_ID,
                                     esp_app_get_description()->project_name, slot->size);
    if (s_last.verdict != FW_UPDATE_INSTALL) {
        (void)fclose(f);
        heap_caps_free(buf);
        reject(path);
        return finish(FW_UPDATE_RESULT_REFUSED, "judge", ESP_OK, start);
    }

    const log_field_t starting[] = {
        log_str("version", s_last.image.version),
        log_str("from_version", esp_app_get_description()->version),
        log_u64("bytes", s_last.file_bytes),
        log_str("slot", slot->label),
    };
    port_log_write(s_log, LOG_LEVEL_INFO, TAG, "firmware on the card: installing", starting,
                   sizeof(starting) / sizeof(starting[0]));

    // Erased as it is written, a sector ahead, rather than in one stretch
    // of seconds before the first byte.
    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        // ESP_ERR_OTA_ROLLBACK_INVALID_STATE: the running image is itself an
        // update not yet confirmed; the next mount, after its first frame.
        (void)fclose(f);
        heap_caps_free(buf);
        return finish(FW_UPDATE_RESULT_FAILED, "begin", err, start);
    }
    size_t since_yield = 0u;
    bool read_failed = false;
    while (n > 0u) {
        err = esp_ota_write(ota, buf, n);
        if (err != ESP_OK) {
            break;
        }
        since_yield += n;
        if (since_yield >= YIELD_EVERY_BYTES) {
            since_yield = 0u;
            vTaskDelay(1);
        }
        n = fread(buf, 1u, CHUNK_BYTES, f);
        if (n == 0u && ferror(f)) {
            read_failed = true;
        }
    }
    (void)fclose(f);
    heap_caps_free(buf);
    if (err != ESP_OK || read_failed) {
        (void)esp_ota_abort(ota);
        return finish(FW_UPDATE_RESULT_FAILED, read_failed ? "read" : "write",
                      read_failed ? (esp_err_t)errno : err, start);
    }

    // ESP-IDF verifies the whole image here: its checksum and appended hash.
    err = esp_ota_end(ota);
    if (err == ESP_ERR_OTA_VALIDATE_FAILED || err == ESP_ERR_INVALID_SIZE) {
        s_last.verdict = FW_UPDATE_REFUSE_INVALID;
        reject(path);
        return finish(FW_UPDATE_RESULT_REFUSED, "end", err, start);
    }
    if (err != ESP_OK) {
        return finish(FW_UPDATE_RESULT_FAILED, "end", err, start);
    }

    // Deleted before the switch: a file that stayed would install again at
    // the next boot's mount, and again after that.
    if (unlink(path) != 0) {
        return finish(FW_UPDATE_RESULT_FAILED, "delete", (esp_err_t)errno, start);
    }
    err = esp_ota_set_boot_partition(slot);
    if (err != ESP_OK) {
        return finish(FW_UPDATE_RESULT_FAILED, "boot", err, start);
    }
    return finish(FW_UPDATE_RESULT_INSTALLED, "", ESP_OK, start);
}
