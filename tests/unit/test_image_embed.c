// SPDX-License-Identifier: Apache-2.0
//
// The picture pipeline end to end: PNG fixtures, embedded at build time by
// apps/tools/image_gen/image_gen.py through cmake/image_gen.cmake -- the tool
// and the helper the slideshow uses -- then read back as image_t and blitted.
//
// Every fixture under tests/unit/fixtures/image_gen/ holds the same 7x3
// picture, level (x + 7y) mod 16 -- all sixteen levels, an odd width -- in a
// different PNG encoding: 4-, 8- and 16-bit grey, a 4-bit palette whose
// entries run backwards, and neutral RGB. All but gray4.png were written by
// Pillow, so the decoder is checked against an encoder other than its own.
// The refusals (an off-level grey, a colour, transparency) are CTest cases of
// their own in tests/CMakeLists.txt.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hac_test.h"
#include "test_images.generated.h"
#include "ui/framebuffer.h"
#include "ui/gfx.h"
#include "ui/image.h"

#define FIXTURE_W 7
#define FIXTURE_H 3

static uint8_t expected_level(int32_t x, int32_t y)
{
    return (uint8_t)((x + FIXTURE_W * y) % 16);
}

static void every_encoding_embeds_the_same_picture(void)
{
    HAC_CHECK_EQ_U64(TEST_IMAGES_COUNT, 5u);

    for (size_t i = 0; i < TEST_IMAGES_COUNT; ++i) {
        const image_t *img = test_images_all[i];
        HAC_CHECK_EQ_INT(img->width, FIXTURE_W);
        HAC_CHECK_EQ_INT(img->height, FIXTURE_H);
        HAC_CHECK_EQ_U64(img->stride, 4u);  // ceil(7 / 2)
        HAC_CHECK(img->format == PIXFMT_GRAY4);

        uint8_t storage[4u * FIXTURE_H];
        framebuffer_t fb;
        framebuffer_init(&fb, storage, sizeof(storage), FIXTURE_W, FIXTURE_H, PIXFMT_GRAY4);
        gfx_blit_image(&fb, 0, 0, img);

        bool same = true;
        for (int32_t y = 0; y < FIXTURE_H; ++y) {
            for (int32_t x = 0; x < FIXTURE_W; ++x) {
                same = same && (framebuffer_get_pixel(&fb, x, y) == expected_level(x, y));
            }
        }
        if (!same) {
            printf("  fixture %s does not hold the expected levels\n", test_images_names[i]);
        }
        HAC_CHECK(same);
    }
}

// docs/adr/0003: the even pixel in the low nibble, the odd one in the high --
// the opposite of PNG's own packing, which embed has to undo. Row 0 holds
// levels 0..6, so its bytes are 0x10, 0x32, 0x54 and then 0x06: level 6 with
// a zero padding nibble above it.
static void bytes_are_in_the_framebuffer_nibble_order(void)
{
    static const uint8_t row0[4] = {0x10, 0x32, 0x54, 0x06};

    for (size_t i = 0; i < TEST_IMAGES_COUNT; ++i) {
        HAC_CHECK(memcmp(test_images_all[i]->pixels, row0, sizeof(row0)) == 0);
    }
}

static void names_are_the_file_stems_in_file_name_order(void)
{
    static const char *const expected[TEST_IMAGES_COUNT] = {
        "gray16", "gray4", "gray8", "palette4", "rgb",
    };

    for (size_t i = 0; i < TEST_IMAGES_COUNT; ++i) {
        HAC_CHECK(strcmp(test_images_names[i], expected[i]) == 0);
    }
    HAC_CHECK(test_images_all[2] == &test_images_gray8);
}

int main(void)
{
    HAC_RUN(every_encoding_embeds_the_same_picture);
    HAC_RUN(bytes_are_in_the_framebuffer_nibble_order);
    HAC_RUN(names_are_the_file_stems_in_file_name_order);
    return hac_test_summary();
}
