// SPDX-License-Identifier: Apache-2.0

#include "fw_image.h"

#include <string.h>

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// A fixed-width field of the app description, which has no NUL when full.
static void copy_field(char *dst, const uint8_t *src, size_t width)
{
    size_t n = 0u;
    while (n < width && src[n] != 0u) {
        dst[n] = (char)src[n];
        ++n;
    }
    dst[n] = '\0';
}

fw_image_status_t fw_image_parse(const uint8_t *head, size_t len, fw_image_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (len < FW_IMAGE_HEAD_BYTES) {
        return FW_IMAGE_TOO_SHORT;
    }
    if (head[0] != FW_IMAGE_MAGIC) {
        return FW_IMAGE_NOT_AN_IMAGE;
    }
    out->chip_id = le16(&head[FW_IMAGE_CHIP_ID_OFFSET]);
    const uint8_t *desc = &head[FW_IMAGE_APP_DESC_OFFSET];
    if (le32(desc) != FW_APP_DESC_MAGIC) {
        return FW_IMAGE_NO_APP_DESC;
    }
    copy_field(out->version, &desc[FW_APP_DESC_VERSION_OFFSET], FW_APP_DESC_STRING_BYTES);
    copy_field(out->project, &desc[FW_APP_DESC_PROJECT_OFFSET], FW_APP_DESC_STRING_BYTES);
    copy_field(out->time, &desc[FW_APP_DESC_TIME_OFFSET], FW_APP_DESC_STAMP_BYTES);
    copy_field(out->date, &desc[FW_APP_DESC_DATE_OFFSET], FW_APP_DESC_STAMP_BYTES);
    copy_field(out->idf_ver, &desc[FW_APP_DESC_IDF_VER_OFFSET], FW_APP_DESC_STRING_BYTES);
    return FW_IMAGE_OK;
}

fw_update_verdict_t fw_update_judge(fw_image_status_t parsed, const fw_image_info_t *info,
                                    uint64_t file_bytes, uint16_t own_chip_id,
                                    const char *own_project, uint64_t slot_bytes)
{
    if (file_bytes == 0u) {
        return FW_UPDATE_REFUSE_EMPTY;
    }
    if (parsed != FW_IMAGE_OK) {
        return FW_UPDATE_REFUSE_NOT_AN_IMAGE;
    }
    if (info->chip_id != own_chip_id) {
        return FW_UPDATE_REFUSE_CHIP;
    }
    // The app description holds 32 bytes of name; compare as many.
    if (strncmp(info->project, own_project, FW_APP_DESC_STRING_BYTES) != 0) {
        return FW_UPDATE_REFUSE_PROJECT;
    }
    if (file_bytes > slot_bytes) {
        return FW_UPDATE_REFUSE_TOO_BIG;
    }
    return FW_UPDATE_INSTALL;
}

const char *fw_update_verdict_name(fw_update_verdict_t verdict)
{
    switch (verdict) {
        case FW_UPDATE_INSTALL:
            return "install";
        case FW_UPDATE_REFUSE_EMPTY:
            return "empty";
        case FW_UPDATE_REFUSE_NOT_AN_IMAGE:
            return "not_an_image";
        case FW_UPDATE_REFUSE_CHIP:
            return "chip";
        case FW_UPDATE_REFUSE_PROJECT:
            return "project";
        case FW_UPDATE_REFUSE_TOO_BIG:
            return "too_big";
        case FW_UPDATE_REFUSE_INVALID:
            return "invalid";
        default:
            return "?";
    }
}
