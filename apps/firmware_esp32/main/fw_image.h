// SPDX-License-Identifier: Apache-2.0
//
// What a firmware file on the card says about itself, and whether the card's
// update installs it (docs/adr/0037-firmware-from-the-card.md). Pure: the
// image's first bytes in, the verdict out, no ESP-IDF header, so the host
// build tests it (tests/unit/test_fw_image.c) as i2c_scan.c is tested;
// fw_update.c checks this file's offsets against ESP-IDF's own structs at
// compile time.
//
// An app image, as ESP-IDF's esp_app_format.h lays it out: a 24-byte image
// header (magic 0xE9, the chip's id at offset 12), the first segment's 8-byte
// header, and at offset 32 the 256-byte app description (magic 0xABCD5432,
// the version at 16, the project's name at 48, time at 80, date at 96, the
// ESP-IDF version at 112), the start of the first segment, where every
// ESP-IDF app keeps it.
//
// The verdict checks what the image is, never which version it is: any image
// of this project for this chip that fits the slot installs -- an older one
// too, deliberately (the ADR, "No version check"). Whether the whole file is
// a sound image is ESP-IDF's to verify while it is written (esp_ota_end()).

#ifndef HAC_APPS_FIRMWARE_ESP32_FW_IMAGE_H
#define HAC_APPS_FIRMWARE_ESP32_FW_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FW_IMAGE_MAGIC 0xE9u
#define FW_IMAGE_HEADER_BYTES 24u
#define FW_IMAGE_CHIP_ID_OFFSET 12u
#define FW_IMAGE_SEGMENT_HEADER_BYTES 8u
#define FW_IMAGE_APP_DESC_OFFSET (FW_IMAGE_HEADER_BYTES + FW_IMAGE_SEGMENT_HEADER_BYTES)

#define FW_APP_DESC_MAGIC 0xABCD5432u
#define FW_APP_DESC_BYTES 256u
#define FW_APP_DESC_VERSION_OFFSET 16u
#define FW_APP_DESC_PROJECT_OFFSET 48u
#define FW_APP_DESC_TIME_OFFSET 80u
#define FW_APP_DESC_DATE_OFFSET 96u
#define FW_APP_DESC_IDF_VER_OFFSET 112u
#define FW_APP_DESC_STRING_BYTES 32u  // version, project, ESP-IDF version
#define FW_APP_DESC_STAMP_BYTES 16u   // time, date

// The bytes fw_image_parse() needs from the file's start.
#define FW_IMAGE_HEAD_BYTES (FW_IMAGE_APP_DESC_OFFSET + FW_APP_DESC_BYTES)

typedef struct {
    uint16_t chip_id;
    // Each NUL-terminated here; in the image a full field has no NUL.
    char version[FW_APP_DESC_STRING_BYTES + 1u];
    char project[FW_APP_DESC_STRING_BYTES + 1u];
    char time[FW_APP_DESC_STAMP_BYTES + 1u];
    char date[FW_APP_DESC_STAMP_BYTES + 1u];
    char idf_ver[FW_APP_DESC_STRING_BYTES + 1u];
} fw_image_info_t;

typedef enum {
    FW_IMAGE_OK = 0,
    FW_IMAGE_TOO_SHORT,     // fewer than FW_IMAGE_HEAD_BYTES
    FW_IMAGE_NOT_AN_IMAGE,  // no image header's magic
    FW_IMAGE_NO_APP_DESC,   // an image, but no app description where ESP-IDF puts one
} fw_image_status_t;

// Reads `head`, the file's first `len` bytes. `out` is filled as far as the
// status allows, zeroed otherwise.
fw_image_status_t fw_image_parse(const uint8_t *head, size_t len, fw_image_info_t *out);

typedef enum {
    FW_UPDATE_INSTALL = 0,
    FW_UPDATE_REFUSE_EMPTY,         // an empty file
    FW_UPDATE_REFUSE_NOT_AN_IMAGE,  // too short, no header, no app description
    FW_UPDATE_REFUSE_CHIP,          // built for another chip
    FW_UPDATE_REFUSE_PROJECT,       // another project's image: the slideshow, a calibration build
    FW_UPDATE_REFUSE_TOO_BIG,       // larger than the slot it would go to
    FW_UPDATE_REFUSE_INVALID,       // ESP-IDF's verification failed: a truncated or damaged file
} fw_update_verdict_t;

// Whether a file of `file_bytes`, parsed as `parsed` into `info`, installs
// into a slot of `slot_bytes` on a chip `own_chip_id` running project
// `own_project`. Never FW_UPDATE_REFUSE_INVALID: that is the write's.
fw_update_verdict_t fw_update_judge(fw_image_status_t parsed, const fw_image_info_t *info,
                                    uint64_t file_bytes, uint16_t own_chip_id,
                                    const char *own_project, uint64_t slot_bytes);

// "install", "empty", "not_an_image", "chip", "project", "too_big",
// "invalid"; "?" for anything else.
const char *fw_update_verdict_name(fw_update_verdict_t verdict);

#ifdef __cplusplus
}
#endif

#endif  // HAC_APPS_FIRMWARE_ESP32_FW_IMAGE_H
