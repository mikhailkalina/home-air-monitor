// SPDX-License-Identifier: Apache-2.0
//
// apps/firmware_esp32/main/fw_image.c, compiled on the host from where the
// firmware compiles it (see tests/CMakeLists.txt): a firmware file's header
// read, and whether the card's update installs it -- no version compared.

#include "fw_image.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hac_test.h"

#define CHIP_S3 0x0009u
#define PROJECT "home_air_monitor"
#define SLOT (4u * 1024u * 1024u)

static uint8_t g_head[FW_IMAGE_HEAD_BYTES + 16u];

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0u; i < 4u; ++i) {
        p[i] = (uint8_t)((v >> (8u * i)) & 0xFFu);
    }
}

static void put_text(size_t desc_offset, const char *text, size_t width)
{
    uint8_t *field = &g_head[FW_IMAGE_APP_DESC_OFFSET + desc_offset];
    memset(field, 0, width);
    const size_t len = strlen(text);
    memcpy(field, text, (len < width) ? len : width);
}

// An image as ESP-IDF's build lays it out, of this project for the S3.
static void image(const char *project, const char *version)
{
    memset(g_head, 0xA5, sizeof(g_head));  // anything outside the fields read
    g_head[0] = FW_IMAGE_MAGIC;
    put_le16(&g_head[FW_IMAGE_CHIP_ID_OFFSET], CHIP_S3);
    put_le32(&g_head[FW_IMAGE_APP_DESC_OFFSET], FW_APP_DESC_MAGIC);
    put_text(FW_APP_DESC_VERSION_OFFSET, version, FW_APP_DESC_STRING_BYTES);
    put_text(FW_APP_DESC_PROJECT_OFFSET, project, FW_APP_DESC_STRING_BYTES);
    put_text(FW_APP_DESC_TIME_OFFSET, "23:36:06", FW_APP_DESC_STAMP_BYTES);
    put_text(FW_APP_DESC_DATE_OFFSET, "Oct 22 2026", FW_APP_DESC_STAMP_BYTES);
    put_text(FW_APP_DESC_IDF_VER_OFFSET, "v5.5", FW_APP_DESC_STRING_BYTES);
}

static void the_layout_is_esp_idfs(void)
{
    HAC_CHECK_EQ_U64(FW_IMAGE_APP_DESC_OFFSET, 32u);
    HAC_CHECK_EQ_U64(FW_IMAGE_HEAD_BYTES, 288u);
    HAC_CHECK(FW_APP_DESC_IDF_VER_OFFSET + FW_APP_DESC_STRING_BYTES <= FW_APP_DESC_BYTES);
}

static void an_image_of_this_project_reads_and_installs(void)
{
    image(PROJECT, "v0.4.0-3-g62944f7");
    fw_image_info_t info;
    const fw_image_status_t st = fw_image_parse(g_head, FW_IMAGE_HEAD_BYTES, &info);
    HAC_CHECK_EQ_INT(st, FW_IMAGE_OK);
    HAC_CHECK_EQ_U64(info.chip_id, CHIP_S3);
    HAC_CHECK(strcmp(info.project, PROJECT) == 0);
    HAC_CHECK(strcmp(info.version, "v0.4.0-3-g62944f7") == 0);
    HAC_CHECK(strcmp(info.time, "23:36:06") == 0);
    HAC_CHECK(strcmp(info.date, "Oct 22 2026") == 0);
    HAC_CHECK(strcmp(info.idf_ver, "v5.5") == 0);
    HAC_CHECK_EQ_INT(fw_update_judge(st, &info, 1432144u, CHIP_S3, PROJECT, SLOT),
                     FW_UPDATE_INSTALL);
}

static void any_version_installs_older_and_dirty_included(void)
{
    static const char *const VERSIONS[] = {"v0.1.0", "8c73fdd-dirty", "", "zzz", "v99.0.0"};
    for (size_t i = 0u; i < sizeof(VERSIONS) / sizeof(VERSIONS[0]); ++i) {
        image(PROJECT, VERSIONS[i]);
        fw_image_info_t info;
        const fw_image_status_t st = fw_image_parse(g_head, FW_IMAGE_HEAD_BYTES, &info);
        HAC_CHECK_EQ_INT(fw_update_judge(st, &info, 1000u, CHIP_S3, PROJECT, SLOT),
                         FW_UPDATE_INSTALL);
    }
}

static void full_width_fields_have_no_nul_and_are_read_whole(void)
{
    const char *v32 = "0123456789abcdef0123456789ABCDEF";  // 32 characters, no room for NUL
    image(PROJECT, v32);
    fw_image_info_t info;
    HAC_CHECK_EQ_INT(fw_image_parse(g_head, FW_IMAGE_HEAD_BYTES, &info), FW_IMAGE_OK);
    HAC_CHECK_EQ_U64(strlen(info.version), 32u);
    HAC_CHECK(memcmp(info.version, v32, 32u) == 0);
}

static void another_project_is_refused(void)
{
    image("hac_slideshow", "v0.4.0");
    fw_image_info_t info;
    const fw_image_status_t st = fw_image_parse(g_head, FW_IMAGE_HEAD_BYTES, &info);
    HAC_CHECK_EQ_INT(fw_update_judge(st, &info, 1000u, CHIP_S3, PROJECT, SLOT),
                     FW_UPDATE_REFUSE_PROJECT);
    // ... a name this project's begins with, too.
    image("home_air_monitor_test", "v0.4.0");
    HAC_CHECK_EQ_INT(fw_update_judge(fw_image_parse(g_head, FW_IMAGE_HEAD_BYTES, &info), &info,
                                     1000u, CHIP_S3, PROJECT, SLOT),
                     FW_UPDATE_REFUSE_PROJECT);
}

static void another_chip_is_refused(void)
{
    image(PROJECT, "v0.4.0");
    put_le16(&g_head[FW_IMAGE_CHIP_ID_OFFSET], 0x0000u);  // the ESP32
    fw_image_info_t info;
    const fw_image_status_t st = fw_image_parse(g_head, FW_IMAGE_HEAD_BYTES, &info);
    HAC_CHECK_EQ_INT(fw_update_judge(st, &info, 1000u, CHIP_S3, PROJECT, SLOT),
                     FW_UPDATE_REFUSE_CHIP);
}

static void a_file_larger_than_the_slot_is_refused_at_the_slot_exactly(void)
{
    image(PROJECT, "v0.4.0");
    fw_image_info_t info;
    const fw_image_status_t st = fw_image_parse(g_head, FW_IMAGE_HEAD_BYTES, &info);
    HAC_CHECK_EQ_INT(fw_update_judge(st, &info, SLOT, CHIP_S3, PROJECT, SLOT), FW_UPDATE_INSTALL);
    HAC_CHECK_EQ_INT(fw_update_judge(st, &info, SLOT + 1u, CHIP_S3, PROJECT, SLOT),
                     FW_UPDATE_REFUSE_TOO_BIG);
}

static void what_is_not_an_image_is_refused(void)
{
    fw_image_info_t info;
    image(PROJECT, "v0.4.0");
    HAC_CHECK_EQ_INT(fw_image_parse(g_head, FW_IMAGE_HEAD_BYTES - 1u, &info), FW_IMAGE_TOO_SHORT);
    HAC_CHECK_EQ_INT(fw_update_judge(FW_IMAGE_TOO_SHORT, &info, 100u, CHIP_S3, PROJECT, SLOT),
                     FW_UPDATE_REFUSE_NOT_AN_IMAGE);

    image(PROJECT, "v0.4.0");
    g_head[0] = 0x50u;  // "PK": someone's zip
    HAC_CHECK_EQ_INT(fw_image_parse(g_head, FW_IMAGE_HEAD_BYTES, &info), FW_IMAGE_NOT_AN_IMAGE);

    image(PROJECT, "v0.4.0");
    put_le32(&g_head[FW_IMAGE_APP_DESC_OFFSET], 0u);  // a bootloader has no app description
    const fw_image_status_t st = fw_image_parse(g_head, FW_IMAGE_HEAD_BYTES, &info);
    HAC_CHECK_EQ_INT(st, FW_IMAGE_NO_APP_DESC);
    HAC_CHECK_EQ_INT(fw_update_judge(st, &info, 20944u, CHIP_S3, PROJECT, SLOT),
                     FW_UPDATE_REFUSE_NOT_AN_IMAGE);

    HAC_CHECK_EQ_INT(fw_update_judge(FW_IMAGE_TOO_SHORT, &info, 0u, CHIP_S3, PROJECT, SLOT),
                     FW_UPDATE_REFUSE_EMPTY);
}

static void verdicts_have_names(void)
{
    HAC_CHECK(strcmp(fw_update_verdict_name(FW_UPDATE_INSTALL), "install") == 0);
    HAC_CHECK(strcmp(fw_update_verdict_name(FW_UPDATE_REFUSE_EMPTY), "empty") == 0);
    HAC_CHECK(strcmp(fw_update_verdict_name(FW_UPDATE_REFUSE_NOT_AN_IMAGE), "not_an_image") == 0);
    HAC_CHECK(strcmp(fw_update_verdict_name(FW_UPDATE_REFUSE_CHIP), "chip") == 0);
    HAC_CHECK(strcmp(fw_update_verdict_name(FW_UPDATE_REFUSE_PROJECT), "project") == 0);
    HAC_CHECK(strcmp(fw_update_verdict_name(FW_UPDATE_REFUSE_TOO_BIG), "too_big") == 0);
    HAC_CHECK(strcmp(fw_update_verdict_name(FW_UPDATE_REFUSE_INVALID), "invalid") == 0);
    HAC_CHECK(strcmp(fw_update_verdict_name((fw_update_verdict_t)99), "?") == 0);
}

int main(void)
{
    HAC_RUN(the_layout_is_esp_idfs);
    HAC_RUN(an_image_of_this_project_reads_and_installs);
    HAC_RUN(any_version_installs_older_and_dirty_included);
    HAC_RUN(full_width_fields_have_no_nul_and_are_read_whole);
    HAC_RUN(another_project_is_refused);
    HAC_RUN(another_chip_is_refused);
    HAC_RUN(a_file_larger_than_the_slot_is_refused_at_the_slot_exactly);
    HAC_RUN(what_is_not_an_image_is_refused);
    HAC_RUN(verdicts_have_names);
    return hac_test_summary();
}
