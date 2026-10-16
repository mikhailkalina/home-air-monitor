// SPDX-License-Identifier: Apache-2.0

#include "ui/screen_home.h"

#include <stddef.h>

#include "ui/fonts/font_dejavu_sans.h"
#include "ui/gfx.h"

// --- layout ----------------------------------------------------------------
//
// Vertical positions are baselines and rule lines, in panel pixels, laid out
// for the 960x540 ED047TC1; the fixed points shared with the layout test are
// in screen_home.h. Horizontal positions come from fb->width, so the four
// reading columns follow a different panel width without editing.
//
// Each baseline sits where the tallest and deepest glyphs of the strings that
// line can show (view_model.h) clear the line above and below by at least
// 7px; the comments give the extents, measured off the fonts.

#define BAR_BASELINE 44  // 40px: 30 above, 8 below ("Battery")

// 40px "CO2 ppm": 30 above, 8 below; public, for the layout test.
#define HEAD_LABEL_BASELINE SCREEN_HOME_HEAD_LABEL_BASELINE
#define HEAD_VALUE_BASELINE 244  // 160px: 119 above, 14 below ("n/a")
#define HEAD_GAP 14

#define BANNER_TEXT_CAP 35  // 48px "Very poor": 35 above the baseline

#define GRID_LABEL_BASELINE 306  // 32px: 24 above, 7 below
#define GRID_VALUE_BASELINE 388  // 72px: 53 above, 7 below ("n/a")
#define GRID_UNIT_BASELINE 434   // 32px units and the 32px mark beside them

#define FOOT_BASELINE 506  // 40px: 30 above, 8 below
#define FOOT_BOX_ABOVE 40  // the error box around the footer's status text
#define FOOT_BOX_BELOW 16

#define MARK_SIZE 16
#define MARK_GAP 12

// The marks are 16px bitmaps, expanded to sit beside text of each size.
#define MARK_SCALE_BAR 2       // beside 40px text
#define MARK_SCALE_GRID 2      // beside the 32px unit
#define MARK_SCALE_HEADLINE 2  // beside the 40px label and unit

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
// value_status_t that is not VAL_OK, so no status can render as a blank, and
// one for a VAL_OK number its source has not calibrated.

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

// A question mark: a reading, on a scale its source has not yet calibrated
// (view_model.h, VM_MARK_LOW_ACCURACY).
static const uint8_t k_mark_low_accuracy[MARK_SIZE * 2] = {
    0x00, 0x00, 0x07, 0xe0, 0x0f, 0xf0, 0x1c, 0x38, 0x18, 0x18, 0x00, 0x18, 0x00, 0x38, 0x00, 0x70,
    0x00, 0xe0, 0x01, 0xc0, 0x01, 0x80, 0x01, 0x80, 0x00, 0x00, 0x01, 0x80, 0x01, 0x80, 0x00, 0x00,
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
// hold its own beside large text is expanded here, one source pixel to a
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
        case VM_MARK_LOW_ACCURACY:
            blit_mark(fb, x, y, k_mark_low_accuracy, scale, ink(fb, gray));
            return;
    }
}

// A horizontal rule across the width inside the margins.
static void draw_rule(framebuffer_t *fb, int32_t y)
{
    gfx_fill_rect(fb, SCREEN_HOME_MARGIN, y, (int32_t)fb->width - 2 * SCREEN_HOME_MARGIN,
                  SCREEN_HOME_RULE_PX, ink(fb, VM_INK_SECONDARY));
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
    const font_t *font = &font_dejavu_sans_40;
    const int32_t right = (int32_t)fb->width - SCREEN_HOME_MARGIN;
    const int32_t mark_px = MARK_SIZE * MARK_SCALE_BAR;

    gfx_draw_text(fb, SCREEN_HOME_MARGIN, BAR_BASELINE, font, "Home Air Monitor",
                  ink(fb, VM_INK_SECONDARY));

    char battery[VM_TEXT_MAX + 16];
    battery_text(battery, sizeof(battery), &vm->battery);

    // Laid out right to left -- link state, battery mark, battery text -- so
    // that a battery value appearing or disappearing does not shift the link
    // text, which would dirty a region that did not actually change.
    const int32_t net_x = right - gfx_measure_text_ink(font, vm->net_text);
    const int32_t mark_x = net_x - MARK_GAP - mark_px;
    const int32_t battery_x = mark_x - MARK_GAP - gfx_measure_text(font, battery);

    (void)gfx_draw_text(fb, battery_x, BAR_BASELINE, font, battery, ink(fb, vm->battery.ink));
    draw_mark_scaled(fb, mark_x, BAR_BASELINE - mark_px, vm->battery.mark, vm->battery.ink,
                     MARK_SCALE_BAR);
    (void)gfx_draw_text(fb, net_x, BAR_BASELINE, font, vm->net_text,
                        ink(fb, vm->net_connected ? VM_INK_PRIMARY : VM_INK_STALE));

    draw_rule(fb, SCREEN_HOME_BAR_RULE_Y);
}

static void draw_headline(framebuffer_t *fb, const view_model_t *vm)
{
    const vm_field_view_t *co2 = &vm->fields[VM_FIELD_CO2];
    const font_t *label_font = &font_dejavu_sans_40;
    const font_t *band_font = &font_dejavu_sans_48;

    // The label, the unit and the mark on one line, each at a place that
    // never moves. The unit used to follow the reading, so every change in
    // its digit count -- 938 to 1002 -- moved it, and a partial refresh
    // drove the letters of each old position: the panel kept them, legible
    // on greys (docs/hardware/board_notes.md, open item "Residue that
    // survives full refreshes"). The mark moved with it. Here nothing but
    // the reading changes when the reading does.
    int32_t x = gfx_draw_text(fb, SCREEN_HOME_MARGIN, HEAD_LABEL_BASELINE, label_font, co2->label,
                              ink(fb, VM_INK_SECONDARY));
    x = gfx_draw_text(fb, x + HEAD_GAP, HEAD_LABEL_BASELINE, label_font, co2->unit,
                      ink(fb, VM_INK_SECONDARY));
    const int32_t mark_px = MARK_SIZE * MARK_SCALE_HEADLINE;
    draw_mark_scaled(fb, x + MARK_GAP, HEAD_LABEL_BASELINE - mark_px, co2->mark, co2->ink,
                     MARK_SCALE_HEADLINE);

    (void)gfx_draw_text(fb, SCREEN_HOME_MARGIN, HEAD_VALUE_BASELINE, &font_dejavu_sans_160,
                        co2->text, ink(fb, co2->ink));

    // The band banner, right aligned. Poor or worse inverts it: the high-CO2
    // warning of docs/requirements.md 9.2 has to be legible across a room.
    const int32_t banner_x = (int32_t)fb->width - SCREEN_HOME_MARGIN - SCREEN_HOME_BANNER_W;
    const uint8_t frame_ink = ink(fb, VM_INK_PRIMARY);

    if (vm->co2_warning) {
        gfx_fill_rect(fb, banner_x, SCREEN_HOME_BANNER_Y, SCREEN_HOME_BANNER_W,
                      SCREEN_HOME_BANNER_H, frame_ink);
    } else {
        for (int32_t i = 0; i < SCREEN_HOME_RULE_PX; ++i) {
            gfx_rect(fb, banner_x + i, SCREEN_HOME_BANNER_Y + i, SCREEN_HOME_BANNER_W - 2 * i,
                     SCREEN_HOME_BANNER_H - 2 * i, ink(fb, VM_INK_SECONDARY));
        }
    }

    const uint8_t banner_text_ink =
        vm->co2_warning ? ink(fb, VM_INK_BACKGROUND) : ink(fb, VM_INK_PRIMARY);
    const int32_t text_w = gfx_measure_text(band_font, vm->co2_level_text);
    const int32_t text_baseline =
        SCREEN_HOME_BANNER_Y + (SCREEN_HOME_BANNER_H + BANNER_TEXT_CAP) / 2;
    gfx_draw_text(fb, banner_x + (SCREEN_HOME_BANNER_W - text_w) / 2, text_baseline, band_font,
                  vm->co2_level_text, banner_text_ink);

    draw_rule(fb, SCREEN_HOME_HEAD_RULE_Y);
}

static void draw_grid(framebuffer_t *fb, const view_model_t *vm)
{
    const int32_t usable = (int32_t)fb->width - 2 * SCREEN_HOME_MARGIN;
    const int32_t column_w = usable / GRID_COLUMNS;
    const int32_t mark_px = MARK_SIZE * MARK_SCALE_GRID;

    for (int32_t col = 0; col < GRID_COLUMNS; ++col) {
        const vm_field_view_t *f = &vm->fields[k_grid_fields[col]];
        const int32_t left = SCREEN_HOME_MARGIN + col * column_w;
        int32_t x = left;

        if (col > 0) {
            // Centred on the boundary between the two columns.
            gfx_fill_rect(
                fb, left - SCREEN_HOME_RULE_PX / 2, SCREEN_HOME_GRID_SEP_TOP, SCREEN_HOME_RULE_PX,
                SCREEN_HOME_GRID_SEP_BOTTOM - SCREEN_HOME_GRID_SEP_TOP, ink(fb, VM_INK_SECONDARY));
            x += SCREEN_HOME_CELL_PAD;
        }

        gfx_draw_text(fb, x, GRID_LABEL_BASELINE, &font_dejavu_sans_32, f->label,
                      ink(fb, VM_INK_SECONDARY));
        (void)gfx_draw_text(fb, x, GRID_VALUE_BASELINE, &font_dejavu_sans_72, f->text,
                            ink(fb, f->ink));

        // The unit under the number rather than after it: a quarter of the
        // panel holds the widest reading at 72px, not that and its unit.
        int32_t ux = x;
        if (f->unit[0] != '\0') {
            ux = gfx_draw_text(fb, x, GRID_UNIT_BASELINE, &font_dejavu_sans_32, f->unit,
                               ink(fb, VM_INK_SECONDARY)) +
                 MARK_GAP;
        }
        draw_mark_scaled(fb, ux, GRID_UNIT_BASELINE - mark_px, f->mark, f->ink, MARK_SCALE_GRID);
    }
}

static void draw_footer(framebuffer_t *fb, const view_model_t *vm)
{
    const font_t *font = &font_dejavu_sans_40;
    const int32_t right = (int32_t)fb->width - SCREEN_HOME_MARGIN;

    draw_rule(fb, SCREEN_HOME_FOOT_RULE_Y);

    // "updated 14:07": a time that stays true for as long as it is on the
    // glass (view_model.h), so nothing has to redraw it.
    const int32_t x = gfx_draw_text(fb, SCREEN_HOME_MARGIN, FOOT_BASELINE, font, "updated ",
                                    ink(fb, VM_INK_SECONDARY));
    (void)gfx_draw_text(fb, x, FOOT_BASELINE, font, vm->updated_text,
                        ink(fb, vm->has_updated ? VM_INK_PRIMARY : VM_INK_UNAVAILABLE));

    const int32_t status_w = gfx_measure_text_ink(font, vm->status_text);
    const int32_t status_x = right - status_w;

    if (vm->error_count > 0u) {
        // Knocked out, matching the error mark, so "2 sensor errors" cannot be
        // mistaken for the ordinary "all sensors OK" line. The box keeps the
        // margin: the text is inset from it by MARK_GAP.
        gfx_fill_rect(fb, status_x - 2 * MARK_GAP, FOOT_BASELINE - FOOT_BOX_ABOVE,
                      status_w + 2 * MARK_GAP, FOOT_BOX_ABOVE + FOOT_BOX_BELOW,
                      ink(fb, VM_INK_PRIMARY));
        (void)gfx_draw_text(fb, status_x - MARK_GAP, FOOT_BASELINE, font, vm->status_text,
                            ink(fb, VM_INK_BACKGROUND));
    } else {
        (void)gfx_draw_text(fb, status_x, FOOT_BASELINE, font, vm->status_text,
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
