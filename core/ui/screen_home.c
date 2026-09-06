// SPDX-License-Identifier: Apache-2.0

#include "ui/screen_home.h"

#include <stddef.h>

#include "ui/fonts/font_dejavu_sans.h"
#include "ui/gfx.h"

// --- layout ----------------------------------------------------------------
//
// Vertical positions are baselines and rule lines, in panel pixels, laid out
// for the 960x540 ED047TC1. Horizontal positions come from fb->width, so the
// four reading columns follow a different panel width without editing.

#define SCREEN_MARGIN 24

#define BAR_BASELINE 34
#define BAR_RULE_Y 50

#define HEAD_LABEL_BASELINE 92
#define HEAD_VALUE_BASELINE 180
#define HEAD_GAP 14
#define HEAD_RULE_Y 214

#define BANNER_Y 110
#define BANNER_H 76
#define BANNER_W 320
#define BANNER_TEXT_BASELINE 162

#define GRID_LABEL_BASELINE 280
#define GRID_VALUE_BASELINE 360
#define GRID_MARK_Y 385
#define GRID_SEP_TOP 236
#define GRID_SEP_BOTTOM 430

#define FOOT_RULE_Y 460
#define FOOT_BASELINE 500

#define MARK_SIZE 16
#define MARK_GAP 12

// The headline mark sits beside a 64px number, so it is drawn at twice the
// size the status bar and the grid use.
#define MARK_SCALE_NORMAL 1
#define MARK_SCALE_HEADLINE 2

// The four quantities under the CO2 headline, in the order they are drawn.
static const vm_field_t k_grid_fields[] = {
    VM_FIELD_TEMPERATURE,
    VM_FIELD_HUMIDITY,
    VM_FIELD_PRESSURE,
    VM_FIELD_IAQ,
};
#define GRID_COLUMNS ((int32_t)(sizeof(k_grid_fields) / sizeof(k_grid_fields[0])))

// --- status marks ----------------------------------------------------------
//
// 16x16, 1bpp, MSB first: the format gfx_blit_glyph1bpp() takes. One per
// value_status_t that is not VAL_OK, so no status can render as a blank.

// A clock face: the value is real but no longer current.
static const uint8_t k_mark_stale[MARK_SIZE * 2] = {
    0x00, 0x00, 0x07, 0xe0, 0x1c, 0x38, 0x30, 0x0c, 0x21, 0x84, 0x61, 0x86, 0x41, 0x82, 0x41, 0xf2,
    0x40, 0xf2, 0x40, 0x02, 0x60, 0x06, 0x20, 0x04, 0x30, 0x0c, 0x1c, 0x38, 0x07, 0xe0, 0x00, 0x00,
};

// A cross, painted in the background ink over a filled square.
static const uint8_t k_mark_error[MARK_SIZE * 2] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x1c, 0x38, 0x0e, 0x70, 0x07, 0xe0, 0x03, 0xc0,
    0x03, 0xc0, 0x07, 0xe0, 0x0e, 0x70, 0x1c, 0x38, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// An hourglass: data is coming, but is not usable yet.
static const uint8_t k_mark_warmup[MARK_SIZE * 2] = {
    0x00, 0x00, 0x3f, 0xfc, 0x3f, 0xfc, 0x1f, 0xf8, 0x0f, 0xf0, 0x07, 0xe0, 0x02, 0x40, 0x01, 0x80,
    0x01, 0x80, 0x02, 0x40, 0x04, 0x20, 0x08, 0x10, 0x10, 0x08, 0x3f, 0xfc, 0x3f, 0xfc, 0x00, 0x00,
};

// A broken bar: this quantity is not provided at all.
static const uint8_t k_mark_unavailable[MARK_SIZE * 2] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0xda, 0x7f, 0xfe,
    0x7f, 0xfe, 0x36, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// --- ink -------------------------------------------------------------------

// view_model_t works in the 16 grey levels of the panel. A 1bpp fast-refresh
// buffer has only ink or no ink, so every grey collapses to ink there.
static uint8_t ink(const framebuffer_t *fb, uint8_t gray)
{
    if (fb->format == PIXFMT_MONO1) {
        return (uint8_t)((gray == VM_INK_BACKGROUND) ? 0u : 1u);
    }
    return gray;
}

// gfx_blit_glyph1bpp() draws at the bitmap's own size, so a mark that has to
// hold its own beside a 64px number is expanded here, one source pixel to a
// scale x scale block.
static void blit_mark(framebuffer_t *fb, int32_t x, int32_t y, const uint8_t *bits, int32_t scale,
                      uint8_t value)
{
    if (scale <= 1) {
        gfx_blit_glyph1bpp(fb, x, y, bits, MARK_SIZE, MARK_SIZE, value);
        return;
    }
    for (int32_t row = 0; row < MARK_SIZE; ++row) {
        for (int32_t col = 0; col < MARK_SIZE; ++col) {
            const uint8_t byte = bits[row * 2 + (col >> 3)];
            if ((byte & (uint8_t)(0x80u >> (col & 7))) != 0u) {
                gfx_fill_rect(fb, x + col * scale, y + row * scale, scale, scale, value);
            }
        }
    }
}

static void draw_mark_scaled(framebuffer_t *fb, int32_t x, int32_t y, vm_mark_t mark, uint8_t gray,
                             int32_t scale)
{
    switch (mark) {
        case VM_MARK_NONE:
            return;
        case VM_MARK_STALE:
            blit_mark(fb, x, y, k_mark_stale, scale, ink(fb, gray));
            return;
        case VM_MARK_ERROR:
            // Inverted: a solid block with the cross knocked out of it, so an
            // error is the loudest thing on an otherwise light screen.
            gfx_fill_rect(fb, x, y, MARK_SIZE * scale, MARK_SIZE * scale, ink(fb, VM_INK_PRIMARY));
            blit_mark(fb, x, y, k_mark_error, scale, ink(fb, VM_INK_BACKGROUND));
            return;
        case VM_MARK_WARMUP:
            blit_mark(fb, x, y, k_mark_warmup, scale, ink(fb, gray));
            return;
        case VM_MARK_UNAVAILABLE:
            blit_mark(fb, x, y, k_mark_unavailable, scale, ink(fb, gray));
            return;
    }
}

static void draw_mark(framebuffer_t *fb, int32_t x, int32_t y, vm_mark_t mark, uint8_t gray)
{
    draw_mark_scaled(fb, x, y, mark, gray, MARK_SCALE_NORMAL);
}

// --- sections --------------------------------------------------------------

// "Battery 87 %", or "Battery ERR" when the gauge has no number to show.
static void battery_text(char *dst, size_t cap, const vm_field_view_t *battery)
{
    size_t len = 0u;

    for (const char *p = "Battery "; *p != '\0' && len + 1u < cap; ++p) {
        dst[len++] = *p;
    }
    for (const char *p = battery->text; *p != '\0' && len + 1u < cap; ++p) {
        dst[len++] = *p;
    }
    if (battery->has_value) {
        for (const char *p = " %"; *p != '\0' && len + 1u < cap; ++p) {
            dst[len++] = *p;
        }
    }
    dst[len] = '\0';
}

static void draw_status_bar(framebuffer_t *fb, const view_model_t *vm)
{
    const int32_t right = (int32_t)fb->width - SCREEN_MARGIN;

    gfx_draw_text(fb, SCREEN_MARGIN, BAR_BASELINE, &font_dejavu_sans_16, "Home Air Monitor",
                  ink(fb, VM_INK_SECONDARY));

    char battery[VM_TEXT_MAX + 16];
    battery_text(battery, sizeof(battery), &vm->battery);

    // Laid out right to left -- link state, battery mark, battery text -- so
    // that a battery value appearing or disappearing does not shift the link
    // text, which would dirty a region that did not actually change.
    const int32_t net_x = right - gfx_measure_text(&font_dejavu_sans_16, vm->net_text);
    const int32_t mark_x = net_x - MARK_GAP - MARK_SIZE;
    const int32_t battery_x = mark_x - MARK_GAP - gfx_measure_text(&font_dejavu_sans_16, battery);

    (void)gfx_draw_text(fb, battery_x, BAR_BASELINE, &font_dejavu_sans_16, battery,
                        ink(fb, vm->battery.ink));
    draw_mark(fb, mark_x, BAR_BASELINE - MARK_SIZE, vm->battery.mark, vm->battery.ink);
    (void)gfx_draw_text(fb, net_x, BAR_BASELINE, &font_dejavu_sans_16, vm->net_text,
                        ink(fb, vm->net_connected ? VM_INK_PRIMARY : VM_INK_STALE));

    gfx_hline(fb, SCREEN_MARGIN, BAR_RULE_Y, (int32_t)fb->width - 2 * SCREEN_MARGIN,
              ink(fb, VM_INK_SECONDARY));
}

static void draw_headline(framebuffer_t *fb, const view_model_t *vm)
{
    const vm_field_view_t *co2 = &vm->fields[VM_FIELD_CO2];

    gfx_draw_text(fb, SCREEN_MARGIN, HEAD_LABEL_BASELINE, &font_dejavu_sans_16, co2->label,
                  ink(fb, VM_INK_SECONDARY));

    int32_t x = gfx_draw_text(fb, SCREEN_MARGIN, HEAD_VALUE_BASELINE, &font_dejavu_sans_64,
                              co2->text, ink(fb, co2->ink));
    x += HEAD_GAP;
    x = gfx_draw_text(fb, x, HEAD_VALUE_BASELINE, &font_dejavu_sans_32, co2->unit,
                      ink(fb, VM_INK_SECONDARY));

    draw_mark_scaled(fb, x + HEAD_GAP, HEAD_VALUE_BASELINE - MARK_SIZE * MARK_SCALE_HEADLINE,
                     co2->mark, co2->ink, MARK_SCALE_HEADLINE);

    // The band banner, right aligned. Poor or worse inverts it: the high-CO2
    // warning of docs/requirements.md 9.2 has to be legible across a room.
    const int32_t banner_x = (int32_t)fb->width - SCREEN_MARGIN - BANNER_W;
    const uint8_t frame_ink = ink(fb, VM_INK_PRIMARY);

    if (vm->co2_warning) {
        gfx_fill_rect(fb, banner_x, BANNER_Y, BANNER_W, BANNER_H, frame_ink);
    } else {
        gfx_rect(fb, banner_x, BANNER_Y, BANNER_W, BANNER_H, ink(fb, VM_INK_SECONDARY));
    }

    const uint8_t banner_text_ink =
        vm->co2_warning ? ink(fb, VM_INK_BACKGROUND) : ink(fb, VM_INK_PRIMARY);
    const int32_t text_w = gfx_measure_text(&font_dejavu_sans_32, vm->co2_level_text);
    gfx_draw_text(fb, banner_x + (BANNER_W - text_w) / 2, BANNER_TEXT_BASELINE,
                  &font_dejavu_sans_32, vm->co2_level_text, banner_text_ink);

    gfx_hline(fb, SCREEN_MARGIN, HEAD_RULE_Y, (int32_t)fb->width - 2 * SCREEN_MARGIN,
              ink(fb, VM_INK_SECONDARY));
}

static void draw_grid(framebuffer_t *fb, const view_model_t *vm)
{
    const int32_t usable = (int32_t)fb->width - 2 * SCREEN_MARGIN;
    const int32_t column_w = usable / GRID_COLUMNS;

    for (int32_t col = 0; col < GRID_COLUMNS; ++col) {
        const vm_field_view_t *f = &vm->fields[k_grid_fields[col]];
        const int32_t x = SCREEN_MARGIN + col * column_w;

        if (col > 0) {
            gfx_vline(fb, x - column_w / 16, GRID_SEP_TOP, GRID_SEP_BOTTOM - GRID_SEP_TOP,
                      ink(fb, VM_INK_SECONDARY));
        }

        gfx_draw_text(fb, x, GRID_LABEL_BASELINE, &font_dejavu_sans_16, f->label,
                      ink(fb, VM_INK_SECONDARY));

        int32_t vx = gfx_draw_text(fb, x, GRID_VALUE_BASELINE, &font_dejavu_sans_32, f->text,
                                   ink(fb, f->ink));
        if (f->unit[0] != '\0') {
            vx += HEAD_GAP / 2;
            (void)gfx_draw_text(fb, vx, GRID_VALUE_BASELINE, &font_dejavu_sans_16, f->unit,
                                ink(fb, VM_INK_SECONDARY));
        }

        draw_mark(fb, x, GRID_MARK_Y, f->mark, f->ink);
    }
}

static void draw_footer(framebuffer_t *fb, const view_model_t *vm)
{
    const int32_t right = (int32_t)fb->width - SCREEN_MARGIN;

    gfx_hline(fb, SCREEN_MARGIN, FOOT_RULE_Y, (int32_t)fb->width - 2 * SCREEN_MARGIN,
              ink(fb, VM_INK_SECONDARY));

    int32_t x = gfx_draw_text(fb, SCREEN_MARGIN, FOOT_BASELINE, &font_dejavu_sans_16, "updated ",
                              ink(fb, VM_INK_SECONDARY));
    x = gfx_draw_text(fb, x, FOOT_BASELINE, &font_dejavu_sans_16, vm->updated_text,
                      ink(fb, vm->has_updated ? VM_INK_PRIMARY : VM_INK_UNAVAILABLE));
    if (vm->has_updated) {
        (void)gfx_draw_text(fb, x, FOOT_BASELINE, &font_dejavu_sans_16, " ago",
                            ink(fb, VM_INK_SECONDARY));
    }

    const int32_t status_w = gfx_measure_text(&font_dejavu_sans_16, vm->status_text);
    const int32_t status_x = right - status_w;

    if (vm->error_count > 0u) {
        // Knocked out, matching the error mark, so "2 sensor errors" cannot be
        // mistaken for the ordinary "all sensors OK" line.
        gfx_fill_rect(fb, status_x - MARK_GAP, FOOT_BASELINE - 20, status_w + 2 * MARK_GAP, 28,
                      ink(fb, VM_INK_PRIMARY));
        (void)gfx_draw_text(fb, status_x, FOOT_BASELINE, &font_dejavu_sans_16, vm->status_text,
                            ink(fb, VM_INK_BACKGROUND));
    } else {
        (void)gfx_draw_text(fb, status_x, FOOT_BASELINE, &font_dejavu_sans_16, vm->status_text,
                            ink(fb, VM_INK_SECONDARY));
    }
}

void screen_home_render(framebuffer_t *fb, const view_model_t *vm)
{
    framebuffer_clear(fb, ink(fb, VM_INK_BACKGROUND));

    draw_status_bar(fb, vm);
    draw_headline(fb, vm);
    draw_grid(fb, vm);
    draw_footer(fb, vm);
}
