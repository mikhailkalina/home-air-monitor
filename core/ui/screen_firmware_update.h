// SPDX-License-Identifier: Apache-2.0
//
// A modal firmware update: actual bytes written, not an estimated total time.
// The application reports stages; the screen controller owns touch locking.
#ifndef HAC_CORE_UI_SCREEN_FIRMWARE_UPDATE_H
#define HAC_CORE_UI_SCREEN_FIRMWARE_UPDATE_H

#include <stdbool.h>
#include <stdint.h>

#include "ui/framebuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FIRMWARE_UPDATE_CHECKING = 0,
    FIRMWARE_UPDATE_WRITING,
    FIRMWARE_UPDATE_VERIFYING,
    FIRMWARE_UPDATE_FINALIZING,
    FIRMWARE_UPDATE_RESTARTING,
    FIRMWARE_UPDATE_FAILED,
    FIRMWARE_UPDATE_REFUSED,
} firmware_update_stage_t;

typedef struct {
    firmware_update_stage_t stage;
    uint64_t written_bytes;
    uint64_t total_bytes;
} firmware_update_progress_t;

// Zero for an unknown/empty file, clamped to 100 for a completed write.
uint8_t firmware_update_percent(const firmware_update_progress_t *progress);
bool firmware_update_active(const firmware_update_progress_t *progress);
const char *firmware_update_stage_name(firmware_update_stage_t stage);
void screen_firmware_update_render(framebuffer_t *fb, const firmware_update_progress_t *progress);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_UI_SCREEN_FIRMWARE_UPDATE_H
