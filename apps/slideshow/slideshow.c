// SPDX-License-Identifier: Apache-2.0

#include "slideshow.h"

#include <stdio.h>
#include <string.h>

#include "ui/fonts/font.h"
#include "ui/fonts/font_dejavu_sans.h"
#include "ui/gfx.h"

// The caption: black text in a white box with a black border, so it reads on
// any picture. Distances in pixels from the panel's bottom-left corner.
#define SLIDESHOW_CAPTION_MARGIN 8
#define SLIDESHOW_CAPTION_PADDING 6
#define SLIDESHOW_CAPTION_INK 0u
#define SLIDESHOW_CAPTION_PAPER 15u
#define SLIDESHOW_CAPTION_MAX 96u

void slideshow_init(slideshow_t *s, const image_t *const *images, const char *const *names,
                    size_t count, const slideshow_config_t *cfg,
                    uint32_t panel_min_full_refresh_interval_ms)
{
    memset(s, 0, sizeof(*s));
    s->images = images;
    s->names = names;
    s->count = count;
    s->cfg = *cfg;
    if (s->cfg.interval_ms < panel_min_full_refresh_interval_ms) {
        s->cfg.interval_ms = panel_min_full_refresh_interval_ms;
    }
}

uint32_t slideshow_interval_ms(const slideshow_t *s)
{
    return s->cfg.interval_ms;
}

uint64_t slideshow_next_due_ms(const slideshow_t *s)
{
    if (!s->have_flushed) {
        return 0u;
    }
    return s->last_flush_ms + (uint64_t)s->cfg.interval_ms;
}

size_t slideshow_current(const slideshow_t *s)
{
    return s->current;
}

const char *slideshow_current_name(const slideshow_t *s)
{
    if (s->names == NULL || s->count == 0u || s->names[s->current] == NULL) {
        return "";
    }
    return s->names[s->current];
}

// How far above (negative) and below the baseline the ink of `text` reaches,
// so the caption box fits the text whatever font it is drawn in.
static void ink_extent(const font_t *font, const char *text, int32_t *top, int32_t *bottom)
{
    *top = 0;
    *bottom = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        if (*p < font->first_codepoint) {
            continue;
        }
        const unsigned int index = (unsigned int)*p - font->first_codepoint;
        if (index >= (unsigned int)font->glyph_count || font->glyphs[index].height == 0u) {
            continue;
        }
        const font_glyph_t *g = &font->glyphs[index];
        if (g->y_offset < *top) {
            *top = g->y_offset;
        }
        if (g->y_offset + (int32_t)g->height > *bottom) {
            *bottom = g->y_offset + (int32_t)g->height;
        }
    }
}

static void draw_caption(const slideshow_t *s, framebuffer_t *fb)
{
    char text[SLIDESHOW_CAPTION_MAX];
    (void)snprintf(text, sizeof(text), "%u/%u  %s", (unsigned)(s->current + 1u), (unsigned)s->count,
                   slideshow_current_name(s));

    const font_t *font = &font_dejavu_sans_16;
    int32_t top = 0;
    int32_t bottom = 0;
    ink_extent(font, text, &top, &bottom);

    const int32_t box_w = gfx_measure_text(font, text) + 2 * SLIDESHOW_CAPTION_PADDING;
    const int32_t box_h = (bottom - top) + 2 * SLIDESHOW_CAPTION_PADDING;
    const int32_t box_x = SLIDESHOW_CAPTION_MARGIN;
    const int32_t box_y = (int32_t)fb->height - SLIDESHOW_CAPTION_MARGIN - box_h;

    gfx_fill_rect(fb, box_x, box_y, box_w, box_h, SLIDESHOW_CAPTION_PAPER);
    gfx_rect(fb, box_x, box_y, box_w, box_h, SLIDESHOW_CAPTION_INK);
    (void)gfx_draw_text(fb, box_x + SLIDESHOW_CAPTION_PADDING,
                        box_y + SLIDESHOW_CAPTION_PADDING - top, font, text, SLIDESHOW_CAPTION_INK);
}

void slideshow_render(const slideshow_t *s, framebuffer_t *fb)
{
    framebuffer_clear(fb, s->cfg.background);
    if (s->count == 0u) {
        return;
    }

    const image_t *img = s->images[s->current];
    const int32_t x = ((int32_t)fb->width - (int32_t)img->width) / 2;
    const int32_t y = ((int32_t)fb->height - (int32_t)img->height) / 2;
    gfx_blit_image(fb, x, y, img);

    if (s->cfg.caption) {
        draw_caption(s, fb);
    }
}

void slideshow_flushed(slideshow_t *s, uint64_t now_ms, bool ok)
{
    s->have_flushed = true;
    s->last_flush_ms = now_ms;
    if (ok && s->count > 0u) {
        s->current = (s->current + 1u) % s->count;
    }
}

size_t slideshow_pixel_bytes(const slideshow_t *s)
{
    size_t total = 0u;
    for (size_t i = 0u; i < s->count; ++i) {
        total += s->images[i]->stride * (size_t)s->images[i]->height;
    }
    return total;
}
