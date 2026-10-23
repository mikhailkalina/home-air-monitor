// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

#include "golden_check.h"
#include "hac_test.h"
#include "ui/framebuffer.h"
#include "ui/screen_firmware_update.h"

#define PANEL_WIDTH 960u
#define PANEL_HEIGHT 540u
static uint8_t g_pixels[PANEL_WIDTH / 2u * PANEL_HEIGHT];

static void render(firmware_update_stage_t stage, uint64_t written, const char *name)
{
    framebuffer_t fb;
    framebuffer_init(&fb, g_pixels, sizeof(g_pixels), PANEL_WIDTH, PANEL_HEIGHT, PIXFMT_GRAY4);
    const firmware_update_progress_t p = {
        .stage = stage, .written_bytes = written, .total_bytes = 1450256u};
    screen_firmware_update_render(&fb, &p);
    HAC_CHECK(golden_check_framebuffer(&fb, HAC_GOLDEN_DIR, HAC_DUMP_DIR, name));
}

static void update_stages_and_failure(void)
{
    render(FIRMWARE_UPDATE_CHECKING, 0u, "screen_update_checking");
    render(FIRMWARE_UPDATE_WRITING, 725128u, "screen_update_writing");
    render(FIRMWARE_UPDATE_VERIFYING, 1450256u, "screen_update_verifying");
    render(FIRMWARE_UPDATE_FINALIZING, 1450256u, "screen_update_finalizing");
    render(FIRMWARE_UPDATE_RESTARTING, 1450256u, "screen_update_restarting");
    render(FIRMWARE_UPDATE_FAILED, 725128u, "screen_update_failed");
    render(FIRMWARE_UPDATE_REFUSED, 0u, "screen_update_refused");
}

int main(void)
{
    HAC_RUN(update_stages_and_failure);
    return hac_test_summary();
}
