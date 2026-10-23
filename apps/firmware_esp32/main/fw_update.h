// SPDX-License-Identifier: Apache-2.0
//
// Firmware from the card (docs/adr/0037-firmware-from-the-card.md): an image
// named FW_UPDATE_FILE_NAME in the card's root is written into the OTA slot
// not running, the file deleted, the slot made the boot one; the caller --
// the card's task, at every mount -- then restarts into it. No version is
// compared (fw_image.h). The new image runs once as ESP-IDF's "pending
// verify" and confirms itself at its first frame (fw_update_confirm_running());
// one that resets before that is rolled back by the bootloader, which this
// project's builds have done since CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE.
//
// A file the update refuses -- another project's, another chip's, too large,
// not an image, or one ESP-IDF's verification rejects -- is renamed to
// FW_UPDATE_FILE_NAME FW_UPDATE_REJECTED_SUFFIX, so a mount does not try it
// again and whoever looks at the card sees what happened; the log says why.
// A file that could not be read or written for another reason stays, for the
// next mount.
//
// fw_update_from_card() runs on the card's task, which owns the card; the
// rest on any task.

#ifndef HAC_APPS_FIRMWARE_ESP32_FW_UPDATE_H
#define HAC_APPS_FIRMWARE_ESP32_FW_UPDATE_H

#include <stdbool.h>
#include <stdint.h>

#include "fw_image.h"
#include "port_log.h"
#include "ui/screen_firmware_update.h"

#ifdef __cplusplus
extern "C" {
#endif

// The name the build gives the image (build/home_air_monitor.bin), in the
// card's root: the file to copy there is the one esptool would flash at
// 0x20000.
#define FW_UPDATE_FILE_NAME "home_air_monitor.bin"
#define FW_UPDATE_REJECTED_SUFFIX ".rejected"

typedef enum {
    FW_UPDATE_RESULT_NONE = 0,   // no file
    FW_UPDATE_RESULT_INSTALLED,  // written and the boot slot: restart into it
    FW_UPDATE_RESULT_REFUSED,    // renamed to .rejected
    FW_UPDATE_RESULT_FAILED,     // the file stays, for the next mount
} fw_update_result_t;

typedef struct {
    fw_update_result_t result;
    fw_update_verdict_t verdict;
    int32_t err;       // of the step that failed: an esp_err_t, an errno for read and delete
    const char *step;  // "display", "memory", "read", "judge", "begin", "write", "end", "delete",
                       // "boot", or ""
    uint64_t file_bytes;
    uint32_t took_ms;
    fw_image_info_t image;
    char slot[17];  // the partition it was written to
} fw_update_attempt_t;

// At boot, after the log: which slot runs, its state and version, and
// whether the other slot holds an image the bootloader rolled back.
void fw_update_boot(const port_log_t *log);

// Runs on the card's task. Returns after the progress reached the panel (or
// a display failure). False aborts before writing further; the source file
// stays for a later retry. Must not access the card from this callback.
typedef bool (*fw_update_progress_fn)(void *ctx, const firmware_update_progress_t *progress);

// The card's task, after each mount of `mount_point`: install the card's
// image if there is one. Takes seconds when there is: the file read, the
// slot erased and written as it goes.
fw_update_result_t fw_update_from_card(const char *mount_point, fw_update_progress_fn progress,
                                       void *progress_ctx);

// The first frame is on the glass: an image running for the first time
// after an update is confirmed, and the bootloader will not roll it back.
// Nothing for one flashed by esptool, or confirmed already.
void fw_update_confirm_running(void);

// The last attempt since boot; false when there was none.
bool fw_update_last_attempt(fw_update_attempt_t *out);

// "none", "installed", "refused", "failed".
const char *fw_update_result_name(fw_update_result_t result);

#ifdef __cplusplus
}
#endif

#endif  // HAC_APPS_FIRMWARE_ESP32_FW_UPDATE_H
