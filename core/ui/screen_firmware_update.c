// SPDX-License-Identifier: Apache-2.0

#include "ui/screen_firmware_update.h"

#include "text/text_append.h"
#include "ui/fonts/font_dejavu_sans.h"
#include "ui/gfx.h"
#include "ui/view_model.h"

// Layout in pixels, centred in the same landscape frame as the other screens.
#define UPDATE_MARGIN 80
#define UPDATE_TITLE_Y 100
#define UPDATE_STAGE_Y 190
#define UPDATE_PERCENT_Y 295
#define UPDATE_BAR_Y 330
#define UPDATE_BAR_HEIGHT 38
#define UPDATE_BAR_INSET 4
#define UPDATE_BYTES_Y 410
#define UPDATE_HINT_Y 480
#define UPDATE_TEXT_BYTES 96u
#define BYTES_PER_KIB 1024u

uint8_t firmware_update_percent(const firmware_update_progress_t *progress)
{
    if (progress->total_bytes == 0u) {
        return 0u;
    }
    if (progress->written_bytes >= progress->total_bytes) {
        return 100u;
    }
    // Avoid overflowing even when a caller reports arbitrary uint64_t sizes.
    // The threshold is ceil(total * percent / 100), split before multiplying.
    for (uint8_t percent = 99u; percent > 0u; --percent) {
        const uint64_t threshold = (progress->total_bytes / 100u) * percent +
                                   ((progress->total_bytes % 100u) * percent + 99u) / 100u;
        if (progress->written_bytes >= threshold) {
            return percent;
        }
    }
    return 0u;
}

bool firmware_update_active(const firmware_update_progress_t *progress)
{
    return progress->stage != FIRMWARE_UPDATE_FAILED && progress->stage != FIRMWARE_UPDATE_REFUSED;
}

const char *firmware_update_stage_name(firmware_update_stage_t stage)
{
    switch (stage) {
        case FIRMWARE_UPDATE_CHECKING:
            return "1 / 4  Checking image";
        case FIRMWARE_UPDATE_WRITING:
            return "2 / 4  Writing firmware";
        case FIRMWARE_UPDATE_VERIFYING:
            return "3 / 4  Verifying image";
        case FIRMWARE_UPDATE_FINALIZING:
            return "4 / 4  Preparing restart";
        case FIRMWARE_UPDATE_RESTARTING:
            return "Restarting...";
        case FIRMWARE_UPDATE_FAILED:
            return "Update failed";
        case FIRMWARE_UPDATE_REFUSED:
            return "Image rejected";
    }
    return "Update failed";
}

static void centred(framebuffer_t *fb, int32_t y, const font_t *font, const char *text, uint8_t ink)
{
    const int32_t x = ((int32_t)fb->width - gfx_measure_text_ink(font, text)) / 2;
    (void)gfx_draw_text(fb, x, y, font, text, ink);
}

void screen_firmware_update_render(framebuffer_t *fb, const firmware_update_progress_t *progress)
{
    const uint8_t background = fb->format == PIXFMT_MONO1 ? 0u : VM_INK_BACKGROUND;
    const uint8_t primary = fb->format == PIXFMT_MONO1 ? 1u : VM_INK_PRIMARY;
    framebuffer_clear(fb, background);
    centred(fb, UPDATE_TITLE_Y, &font_dejavu_sans_48, "Firmware update", primary);
    centred(fb, UPDATE_STAGE_Y, &font_dejavu_sans_32, firmware_update_stage_name(progress->stage),
            primary);

    char text[UPDATE_TEXT_BYTES];
    size_t len = text_append_u64(text, sizeof(text), 0u, firmware_update_percent(progress), 0u);
    (void)text_append_str(text, sizeof(text), len, "% written");
    centred(fb, UPDATE_PERCENT_Y, &font_dejavu_sans_48, text, primary);

    const int32_t width = (int32_t)fb->width - 2 * UPDATE_MARGIN;
    gfx_rect(fb, UPDATE_MARGIN, UPDATE_BAR_Y, width, UPDATE_BAR_HEIGHT, primary);
    const int32_t fill = (width - 2 * UPDATE_BAR_INSET) * firmware_update_percent(progress) / 100;
    gfx_fill_rect(fb, UPDATE_MARGIN + UPDATE_BAR_INSET, UPDATE_BAR_Y + UPDATE_BAR_INSET, fill,
                  UPDATE_BAR_HEIGHT - 2 * UPDATE_BAR_INSET, primary);

    if (progress->total_bytes > 0u) {
        len = text_append_u64(text, sizeof(text), 0u, progress->written_bytes / BYTES_PER_KIB, 0u);
        len = text_append_str(text, sizeof(text), len, " / ");
        len = text_append_u64(text, sizeof(text), len, progress->total_bytes / BYTES_PER_KIB, 0u);
        (void)text_append_str(text, sizeof(text), len, " KiB");
        centred(fb, UPDATE_BYTES_Y, &font_dejavu_sans_32, text, primary);
    }
    centred(fb, UPDATE_HINT_Y, &font_dejavu_sans_32,
            firmware_update_active(progress) ? "Keep power on. Touch is locked." : "Tap to return",
            primary);
}
