// SPDX-License-Identifier: Apache-2.0

#include "ui/screen_diagnostics.h"

#include <stddef.h>

#include "ui/fonts/font_dejavu_sans.h"
#include "ui/gfx.h"
#include "ui/view_model.h"

#define TEXT_MAX 48

// A GRAY4 grey, or ink / no ink on a 1bpp buffer, as screen_home.c does.
static uint8_t ink(const framebuffer_t *fb, uint8_t gray)
{
    if (fb->format == PIXFMT_MONO1) {
        return (uint8_t)((gray == VM_INK_BACKGROUND) ? 0u : 1u);
    }
    return gray;
}

// What is wrong in black, what is well in grey.
static uint8_t tone(const framebuffer_t *fb, bool trouble)
{
    return ink(fb, trouble ? VM_INK_PRIMARY : VM_INK_SECONDARY);
}

static void draw_rule(framebuffer_t *fb, int32_t y)
{
    gfx_fill_rect(fb, SCREEN_DIAG_MARGIN, y, (int32_t)fb->width - 2 * SCREEN_DIAG_MARGIN,
                  SCREEN_DIAG_RULE_PX, ink(fb, VM_INK_SECONDARY));
}

// The longest beginning of `text` whose ink fits `max_px`.
static void fit(char *dst, size_t cap, const font_t *font, const char *text, int32_t max_px)
{
    size_t n = 0u;
    dst[0] = '\0';
    while (text[n] != '\0' && n + 1u < cap) {
        dst[n] = text[n];
        dst[n + 1u] = '\0';
        if (gfx_measure_text_ink(font, dst) > max_px) {
            dst[n] = '\0';
            return;
        }
        ++n;
    }
}

static void join(char *dst, size_t cap, const char *a, const char *b)
{
    size_t len = 0u;
    for (const char *p = a; *p != '\0' && len + 1u < cap; ++p) {
        dst[len++] = *p;
    }
    for (const char *p = b; *p != '\0' && len + 1u < cap; ++p) {
        dst[len++] = *p;
    }
    dst[len] = '\0';
}

static void draw_title(framebuffer_t *fb, const diag_view_t *v)
{
    const font_t *font = &font_dejavu_sans_32;
    const int32_t right = (int32_t)fb->width - SCREEN_DIAG_MARGIN;
    (void)gfx_draw_text(fb, SCREEN_DIAG_MARGIN, SCREEN_DIAG_TITLE_BASELINE, font, "Diagnostics",
                        ink(fb, VM_INK_SECONDARY));
    const char *hint = "tap to return";
    const int32_t hint_x = right - gfx_measure_text_ink(font, hint);
    (void)gfx_draw_text(fb, hint_x, SCREEN_DIAG_TITLE_BASELINE, font, hint,
                        ink(fb, VM_INK_SECONDARY));
    // The status bar's word for the link, where the bar has it: what redraws
    // the network's lines (diagnostics_view.h).
    const char *word = (v->link_text != NULL) ? v->link_text : "";
    const int32_t word_x = hint_x - SCREEN_DIAG_TITLE_GAP - gfx_measure_text_ink(font, word);
    (void)gfx_draw_text(fb, word_x, SCREEN_DIAG_TITLE_BASELINE, font, word,
                        tone(fb, v->link_trouble));
    draw_rule(fb, SCREEN_DIAG_TITLE_RULE_Y);
}

static void draw_slots(framebuffer_t *fb, const diag_view_t *v)
{
    const font_t *font = &font_dejavu_sans_32;
    if (v->slot_count == 0u) {
        (void)gfx_draw_text(fb, SCREEN_DIAG_SLOT_NAME_X, SCREEN_DIAG_SLOTS_BASELINE, font,
                            "no sensor sources", ink(fb, VM_INK_PRIMARY));
    }
    for (size_t i = 0u; i < v->slot_count && i < DIAG_MAX_SLOTS; ++i) {
        const diag_slot_view_t *s = &v->slots[i];
        const int32_t y = SCREEN_DIAG_SLOTS_BASELINE + (int32_t)i * SCREEN_DIAG_PITCH;
        char text[TEXT_MAX];

        fit(text, sizeof(text), font, s->name,
            SCREEN_DIAG_SLOT_STATE_X - SCREEN_DIAG_COLUMN_GAP - SCREEN_DIAG_SLOT_NAME_X);
        (void)gfx_draw_text(fb, SCREEN_DIAG_SLOT_NAME_X, y, font, text, ink(fb, VM_INK_PRIMARY));
        (void)gfx_draw_text(fb, SCREEN_DIAG_SLOT_STATE_X, y, font, s->state, tone(fb, s->trouble));

        const bool failed = !(s->failures[0] == '0' && s->failures[1] == '\0');
        join(text, sizeof(text), "failures ", s->failures);
        (void)gfx_draw_text(fb, SCREEN_DIAG_SLOT_FAILURES_X, y, font, text, tone(fb, failed));
        const bool restarted = !(s->restarts[0] == '0' && s->restarts[1] == '\0');
        join(text, sizeof(text), "restarts ", s->restarts);
        (void)gfx_draw_text(fb, SCREEN_DIAG_SLOT_RESTARTS_X, y, font, text, tone(fb, restarted));
    }
    draw_rule(fb, SCREEN_DIAG_SLOTS_RULE_Y);
}

static void draw_fields(framebuffer_t *fb, const diag_view_t *v)
{
    const font_t *font = &font_dejavu_sans_32;
    const int32_t right = (int32_t)fb->width - SCREEN_DIAG_MARGIN;
    for (size_t i = 0u; i < DIAG_FIELD_COUNT; ++i) {
        const diag_field_view_t *f = &v->fields[i];
        const int32_t y = SCREEN_DIAG_FIELDS_BASELINE + (int32_t)i * SCREEN_DIAG_PITCH;

        (void)gfx_draw_text(fb, SCREEN_DIAG_FIELD_LABEL_X, y, font, f->label,
                            ink(fb, VM_INK_PRIMARY));
        (void)gfx_draw_text(fb, SCREEN_DIAG_FIELD_STATUS_X, y, font, f->status,
                            tone(fb, f->trouble));
        (void)gfx_draw_text(fb, SCREEN_DIAG_FIELD_ACCURACY_X, y, font, f->accuracy,
                            tone(fb, f->trouble));

        char from[TEXT_MAX];
        if (f->source[0] == '\0') {
            join(from, sizeof(from), "-", "");
        } else {
            char name[TEXT_MAX];
            const int32_t prefix = gfx_measure_text(font, "from ");
            fit(name, sizeof(name), font, f->source, right - SCREEN_DIAG_FIELD_SOURCE_X - prefix);
            join(from, sizeof(from), "from ", name);
        }
        (void)gfx_draw_text(fb, SCREEN_DIAG_FIELD_SOURCE_X, y, font, from,
                            ink(fb, VM_INK_SECONDARY));
    }
    draw_rule(fb, SCREEN_DIAG_FIELDS_RULE_Y);
}

// The text right-aligned at the margin, as the last column of a line.
static void draw_right(framebuffer_t *fb, int32_t y, const char *text, uint8_t value)
{
    const font_t *font = &font_dejavu_sans_32;
    const int32_t right = (int32_t)fb->width - SCREEN_DIAG_MARGIN;
    (void)gfx_draw_text(fb, right - gfx_measure_text_ink(font, text), y, font, text, value);
}

static void draw_network(framebuffer_t *fb, const diag_view_t *v)
{
    const font_t *font = &font_dejavu_sans_32;
    const int32_t wifi_y = SCREEN_DIAG_NET_BASELINE;
    const int32_t mqtt_y = SCREEN_DIAG_NET_BASELINE + SCREEN_DIAG_PITCH;
    const uint8_t well = ink(fb, VM_INK_SECONDARY);

    (void)gfx_draw_text(fb, SCREEN_DIAG_NET_LABEL_X, wifi_y, font, "Wi-Fi",
                        ink(fb, VM_INK_PRIMARY));
    (void)gfx_draw_text(fb, SCREEN_DIAG_NET_STATE_X, wifi_y, font, v->wifi_state,
                        tone(fb, v->wifi_trouble));
    char name[TEXT_MAX];
    fit(name, sizeof(name), font, v->ssid,
        SCREEN_DIAG_NET_COUNT_X - SCREEN_DIAG_COLUMN_GAP - SCREEN_DIAG_NET_DETAIL_X);
    (void)gfx_draw_text(fb, SCREEN_DIAG_NET_DETAIL_X, wifi_y, font, name, well);
    (void)gfx_draw_text(fb, SCREEN_DIAG_NET_COUNT_X, wifi_y, font, v->rssi, well);
    draw_right(fb, wifi_y, v->address, well);

    (void)gfx_draw_text(fb, SCREEN_DIAG_NET_LABEL_X, mqtt_y, font, "MQTT", ink(fb, VM_INK_PRIMARY));
    (void)gfx_draw_text(fb, SCREEN_DIAG_NET_STATE_X, mqtt_y, font, v->broker_state,
                        tone(fb, v->broker_trouble));
    (void)gfx_draw_text(fb, SCREEN_DIAG_NET_DETAIL_X, mqtt_y, font, v->sent, well);
    (void)gfx_draw_text(fb, SCREEN_DIAG_NET_COUNT_X, mqtt_y, font, v->refused,
                        tone(fb, v->refused_trouble));
    draw_right(fb, mqtt_y, v->last, well);
}

static void draw_foot(framebuffer_t *fb, const diag_view_t *v)
{
    const font_t *font = &font_dejavu_sans_32;
    const int32_t right = (int32_t)fb->width - SCREEN_DIAG_MARGIN;
    (void)gfx_draw_text(fb, SCREEN_DIAG_MARGIN, SCREEN_DIAG_FOOT_BASELINE, font, v->clock_text,
                        tone(fb, !v->clock_set));
    const int32_t x = right - gfx_measure_text_ink(font, v->recorder_text);
    (void)gfx_draw_text(fb, x, SCREEN_DIAG_FOOT_BASELINE, font, v->recorder_text,
                        tone(fb, v->recorder_trouble));
}

void screen_diagnostics_render(framebuffer_t *fb, const diag_view_t *v)
{
    framebuffer_clear(fb, ink(fb, VM_INK_BACKGROUND));
    draw_title(fb, v);
    draw_slots(fb, v);
    draw_fields(fb, v);
    draw_network(fb, v);
    draw_foot(fb, v);
}
