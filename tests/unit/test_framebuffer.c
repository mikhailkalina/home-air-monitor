// SPDX-License-Identifier: Apache-2.0
//
// framebuffer_t owns no memory and does no drawing of its own; what has to be
// pinned down here is the packing (which nibble/bit is which pixel), the
// bounds clipping, and the dirty-rect bookkeeping that gfx.c and, later, a
// display adapter both rely on.

#include "hac_test.h"
#include "ui/framebuffer.h"

// --- sizing ------------------------------------------------------------

static void required_size_packs_two_gray4_pixels_per_byte(void)
{
    HAC_CHECK_EQ_U64(framebuffer_required_size(4, 2, PIXFMT_GRAY4), 4u);  // 2 bytes/row * 2 rows
    HAC_CHECK_EQ_U64(framebuffer_required_size(5, 2, PIXFMT_GRAY4), 6u);  // odd width rounds up
    HAC_CHECK_EQ_U64(framebuffer_required_size(1, 1, PIXFMT_GRAY4), 1u);
    HAC_CHECK_EQ_U64(framebuffer_required_size(0, 5, PIXFMT_GRAY4), 0u);
}

static void required_size_packs_eight_mono1_pixels_per_byte(void)
{
    HAC_CHECK_EQ_U64(framebuffer_required_size(8, 2, PIXFMT_MONO1), 2u);
    HAC_CHECK_EQ_U64(framebuffer_required_size(9, 2, PIXFMT_MONO1),
                     4u);  // rounds up to 2 bytes/row
    HAC_CHECK_EQ_U64(framebuffer_required_size(1, 1, PIXFMT_MONO1), 1u);
}

// --- init ----------------------------------------------------------------

static void init_wraps_a_correctly_sized_buffer(void)
{
    uint8_t storage[4] = {0};
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), 4, 2, PIXFMT_GRAY4);

    HAC_CHECK_EQ_INT(fb.width, 4);
    HAC_CHECK_EQ_INT(fb.height, 2);
    HAC_CHECK_EQ_U64(fb.stride, 2u);
    HAC_CHECK(fb.pixels == storage);
    HAC_CHECK(!fb.dirty);
}

static void init_leaves_a_zero_sized_buffer_on_a_too_small_backing_store(void)
{
    uint8_t storage[3] = {0};  // one byte short of the 4 required
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), 4, 2, PIXFMT_GRAY4);

    HAC_CHECK_EQ_INT(fb.width, 0);
    HAC_CHECK_EQ_INT(fb.height, 0);
    HAC_CHECK(fb.pixels == NULL);

    // Every later call clips to nothing instead of touching `storage`.
    framebuffer_set_pixel(&fb, 0, 0, 5);
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 0, 0), 0);
    HAC_CHECK(!fb.dirty);
}

static void init_leaves_a_zero_sized_buffer_on_a_null_backing_store(void)
{
    framebuffer_t fb;
    framebuffer_init(&fb, NULL, 0, 4, 2, PIXFMT_GRAY4);

    HAC_CHECK(fb.pixels == NULL);
    framebuffer_set_pixel(&fb, 0, 0, 5);  // must not crash
    HAC_CHECK(!fb.dirty);
}

// --- GRAY4 packing -------------------------------------------------------

static void gray4_even_and_odd_pixels_share_a_byte_independently(void)
{
    uint8_t storage[1] = {0};
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), 2, 1, PIXFMT_GRAY4);

    framebuffer_set_pixel(&fb, 0, 0, 0xA);  // high nibble
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 0, 0), 0xA);
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 1, 0), 0x0);  // untouched
    HAC_CHECK_EQ_INT(storage[0], 0xA0);

    framebuffer_set_pixel(&fb, 1, 0, 0x5);                    // low nibble
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 0, 0), 0xA);  // survives the neighbour write
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 1, 0), 0x5);
    HAC_CHECK_EQ_INT(storage[0], 0xA5);
}

static void gray4_value_is_masked_to_four_bits(void)
{
    uint8_t storage[1] = {0};
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), 1, 1, PIXFMT_GRAY4);

    framebuffer_set_pixel(&fb, 0, 0, 0xFF);
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 0, 0), 0x0F);
}

// --- MONO1 packing ---------------------------------------------------------

static void mono1_packs_msb_first(void)
{
    uint8_t storage[1] = {0};
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), 8, 1, PIXFMT_MONO1);

    framebuffer_set_pixel(&fb, 0, 0, 1);  // bit 0x80
    framebuffer_set_pixel(&fb, 7, 0, 1);  // bit 0x01
    HAC_CHECK_EQ_INT(storage[0], 0x81);

    for (int32_t x = 1; x < 7; ++x) {
        HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, x, 0), 0);
    }
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 0, 0), 1);
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 7, 0), 1);
}

static void mono1_clearing_a_bit_leaves_its_neighbours_alone(void)
{
    uint8_t storage[1] = {0xFF};
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), 8, 1, PIXFMT_MONO1);

    framebuffer_set_pixel(&fb, 3, 0, 0);
    HAC_CHECK_EQ_INT(storage[0], 0xEF);
}

// --- bounds clipping -------------------------------------------------------

static void out_of_range_coordinates_are_a_silent_no_op(void)
{
    uint8_t storage[4] = {0};
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), 4, 2, PIXFMT_GRAY4);

    framebuffer_set_pixel(&fb, -1, 0, 0xF);
    framebuffer_set_pixel(&fb, 0, -1, 0xF);
    framebuffer_set_pixel(&fb, 4, 0, 0xF);
    framebuffer_set_pixel(&fb, 0, 2, 0xF);

    HAC_CHECK(!fb.dirty);
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, -1, 0), 0);
    HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, 4, 0), 0);
}

// --- clear -----------------------------------------------------------------

static void clear_fills_every_pixel_and_marks_the_whole_buffer_dirty(void)
{
    uint8_t storage[4] = {0};  // 4x2 GRAY4
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), 4, 2, PIXFMT_GRAY4);

    framebuffer_clear(&fb, 0x3);

    for (int32_t y = 0; y < 2; ++y) {
        for (int32_t x = 0; x < 4; ++x) {
            HAC_CHECK_EQ_INT(framebuffer_get_pixel(&fb, x, y), 0x3);
        }
    }

    HAC_CHECK(fb.dirty);
    HAC_CHECK_EQ_INT(fb.dirty_rect.x, 0);
    HAC_CHECK_EQ_INT(fb.dirty_rect.y, 0);
    HAC_CHECK_EQ_INT(fb.dirty_rect.w, 4);
    HAC_CHECK_EQ_INT(fb.dirty_rect.h, 2);
}

static void clear_on_mono1_treats_any_nonzero_value_as_set(void)
{
    uint8_t storage[1] = {0};
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), 8, 1, PIXFMT_MONO1);

    framebuffer_clear(&fb, 7);  // nonzero, not just 1
    HAC_CHECK_EQ_INT(storage[0], 0xFF);
}

// --- dirty-rect tracking -----------------------------------------------

static void a_fresh_framebuffer_is_not_dirty(void)
{
    uint8_t storage[4] = {0};
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), 4, 2, PIXFMT_GRAY4);

    HAC_CHECK(!fb.dirty);
}

static void the_first_write_makes_a_one_pixel_dirty_rect(void)
{
    uint8_t storage[4] = {0};
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), 4, 2, PIXFMT_GRAY4);

    framebuffer_set_pixel(&fb, 2, 1, 5);

    HAC_CHECK(fb.dirty);
    HAC_CHECK_EQ_INT(fb.dirty_rect.x, 2);
    HAC_CHECK_EQ_INT(fb.dirty_rect.y, 1);
    HAC_CHECK_EQ_INT(fb.dirty_rect.w, 1);
    HAC_CHECK_EQ_INT(fb.dirty_rect.h, 1);
}

static void the_dirty_rect_widens_to_the_bounding_box_of_every_write(void)
{
    uint8_t storage[50];  // 10x10 GRAY4: 5 bytes/row * 10 rows
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), 10, 10, PIXFMT_GRAY4);

    framebuffer_set_pixel(&fb, 5, 5, 1);
    framebuffer_set_pixel(&fb, 2, 8, 1);
    framebuffer_set_pixel(&fb, 7, 3, 1);

    // Bounding box of (5,5), (2,8), (7,3): x in [2,8), y in [3,9).
    HAC_CHECK_EQ_INT(fb.dirty_rect.x, 2);
    HAC_CHECK_EQ_INT(fb.dirty_rect.y, 3);
    HAC_CHECK_EQ_INT(fb.dirty_rect.w, 6);
    HAC_CHECK_EQ_INT(fb.dirty_rect.h, 6);
}

static void writing_the_same_pixel_twice_does_not_widen_the_rect(void)
{
    uint8_t storage[4] = {0};
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), 4, 2, PIXFMT_GRAY4);

    framebuffer_set_pixel(&fb, 1, 1, 1);
    framebuffer_set_pixel(&fb, 1, 1, 2);

    HAC_CHECK_EQ_INT(fb.dirty_rect.w, 1);
    HAC_CHECK_EQ_INT(fb.dirty_rect.h, 1);
}

static void reset_dirty_clears_the_flag_and_rect(void)
{
    uint8_t storage[4] = {0};
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), 4, 2, PIXFMT_GRAY4);

    framebuffer_set_pixel(&fb, 1, 1, 1);
    framebuffer_reset_dirty(&fb);

    HAC_CHECK(!fb.dirty);

    // A write after reset starts a fresh, tight bounding box rather than
    // resuming the one before the reset.
    framebuffer_set_pixel(&fb, 3, 0, 1);
    HAC_CHECK_EQ_INT(fb.dirty_rect.x, 3);
    HAC_CHECK_EQ_INT(fb.dirty_rect.y, 0);
    HAC_CHECK_EQ_INT(fb.dirty_rect.w, 1);
    HAC_CHECK_EQ_INT(fb.dirty_rect.h, 1);
}

// An out-of-range write must not be able to touch the dirty rect at all,
// since it never touches a pixel either.
static void out_of_range_writes_do_not_affect_the_dirty_rect(void)
{
    uint8_t storage[4] = {0};
    framebuffer_t fb;
    framebuffer_init(&fb, storage, sizeof(storage), 4, 2, PIXFMT_GRAY4);

    framebuffer_set_pixel(&fb, 1, 1, 1);
    framebuffer_set_pixel(&fb, 100, 100, 1);

    HAC_CHECK_EQ_INT(fb.dirty_rect.w, 1);
    HAC_CHECK_EQ_INT(fb.dirty_rect.h, 1);
}

int main(void)
{
    HAC_RUN(required_size_packs_two_gray4_pixels_per_byte);
    HAC_RUN(required_size_packs_eight_mono1_pixels_per_byte);

    HAC_RUN(init_wraps_a_correctly_sized_buffer);
    HAC_RUN(init_leaves_a_zero_sized_buffer_on_a_too_small_backing_store);
    HAC_RUN(init_leaves_a_zero_sized_buffer_on_a_null_backing_store);

    HAC_RUN(gray4_even_and_odd_pixels_share_a_byte_independently);
    HAC_RUN(gray4_value_is_masked_to_four_bits);

    HAC_RUN(mono1_packs_msb_first);
    HAC_RUN(mono1_clearing_a_bit_leaves_its_neighbours_alone);

    HAC_RUN(out_of_range_coordinates_are_a_silent_no_op);

    HAC_RUN(clear_fills_every_pixel_and_marks_the_whole_buffer_dirty);
    HAC_RUN(clear_on_mono1_treats_any_nonzero_value_as_set);

    HAC_RUN(a_fresh_framebuffer_is_not_dirty);
    HAC_RUN(the_first_write_makes_a_one_pixel_dirty_rect);
    HAC_RUN(the_dirty_rect_widens_to_the_bounding_box_of_every_write);
    HAC_RUN(writing_the_same_pixel_twice_does_not_widen_the_rect);
    HAC_RUN(reset_dirty_clears_the_flag_and_rect);
    HAC_RUN(out_of_range_writes_do_not_affect_the_dirty_rect);

    return hac_test_summary();
}
