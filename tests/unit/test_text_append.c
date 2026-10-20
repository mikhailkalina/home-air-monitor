// SPDX-License-Identifier: Apache-2.0
//
// core/text/text_append.c: the text the recordings and the MQTT messages are
// built from, byte for byte, and what happens at the edge of the buffer.

#include "text/text_append.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "hac_test.h"

static void fixed_point_rounds_half_away_and_never_writes_negative_zero(void)
{
    char buf[32];
    HAC_CHECK_EQ_U64(text_append_fixed(buf, sizeof(buf), 0u, 21.5, 2u), 5u);
    HAC_CHECK(strcmp(buf, "21.50") == 0);
    (void)text_append_fixed(buf, sizeof(buf), 0u, -5.255, 1u);
    HAC_CHECK(strcmp(buf, "-5.3") == 0);
    (void)text_append_fixed(buf, sizeof(buf), 0u, -0.004, 2u);
    HAC_CHECK(strcmp(buf, "0.00") == 0);
    (void)text_append_fixed(buf, sizeof(buf), 0u, -0.0, 2u);
    HAC_CHECK(strcmp(buf, "0.00") == 0);
    (void)text_append_fixed(buf, sizeof(buf), 0u, -0.005, 2u);
    HAC_CHECK(strcmp(buf, "-0.01") == 0);
    (void)text_append_fixed(buf, sizeof(buf), 0u, 0.4, 0u);
    HAC_CHECK(strcmp(buf, "0") == 0);
    (void)text_append_fixed(buf, sizeof(buf), 0u, 1006.0625, 3u);
    HAC_CHECK(strcmp(buf, "1006.063") == 0);
    (void)text_append_fixed(buf, sizeof(buf), 0u, 0.05, 2u);
    HAC_CHECK(strcmp(buf, "0.05") == 0);
    (void)text_append_fixed(buf, sizeof(buf), 0u, -12.25, 2u);
    HAC_CHECK(strcmp(buf, "-12.25") == 0);
}

static void a_number_that_cannot_be_written_is_refused_not_approximated(void)
{
    char buf[32];
    HAC_CHECK(!text_fixed_representable(NAN, 2u));
    HAC_CHECK(!text_fixed_representable(INFINITY, 0u));
    HAC_CHECK(!text_fixed_representable(-INFINITY, 0u));
    HAC_CHECK(!text_fixed_representable(1e20, 0u));
    HAC_CHECK(!text_fixed_representable(1.0, 11u));
    HAC_CHECK(text_fixed_representable(1e15, 0u));
    HAC_CHECK(text_fixed_representable(-1e15, 0u));
    HAC_CHECK(!text_fixed_representable(1e15, 1u));

    // Not a number, or too large: nothing a reader could mistake for one.
    HAC_CHECK_EQ_U64(text_append_fixed(buf, sizeof(buf), 0u, NAN, 2u), sizeof(buf));
    HAC_CHECK_EQ_U64(text_append_fixed(buf, sizeof(buf), 0u, INFINITY, 2u), sizeof(buf));
    HAC_CHECK_EQ_U64(text_append_fixed(buf, sizeof(buf), 0u, 1e20, 0u), sizeof(buf));
    // Out of room: the cap, never a partial number.
    HAC_CHECK_EQ_U64(text_append_fixed(buf, 4u, 0u, 1234.5, 1u), 4u);
}

static void signed_integers_cover_the_whole_range(void)
{
    char buf[32];
    HAC_CHECK_EQ_U64(text_append_i64(buf, sizeof(buf), 0u, -61), 3u);
    HAC_CHECK(strcmp(buf, "-61") == 0);
    (void)text_append_i64(buf, sizeof(buf), 0u, 0);
    HAC_CHECK(strcmp(buf, "0") == 0);
    (void)text_append_i64(buf, sizeof(buf), 0u, INT64_MAX);
    HAC_CHECK(strcmp(buf, "9223372036854775807") == 0);
    (void)text_append_i64(buf, sizeof(buf), 0u, INT64_MIN);
    HAC_CHECK(strcmp(buf, "-9223372036854775808") == 0);
}

static void text_exactly_the_buffer_fits_and_a_byte_less_does_not(void)
{
    char buf[8];
    // "abc-61" is six characters: seven bytes with the terminator.
    size_t len = text_append_str(buf, 7u, 0u, "abc");
    len = text_append_i64(buf, 7u, len, -61);
    HAC_CHECK_EQ_U64(len, 6u);
    HAC_CHECK(strcmp(buf, "abc-61") == 0);
    HAC_CHECK(!text_overflowed(len, 7u));

    len = text_append_str(buf, 6u, 0u, "abc");
    len = text_append_i64(buf, 6u, len, -61);
    HAC_CHECK(text_overflowed(len, 6u));
    // Still terminated, and an overflow is passed on by whatever follows.
    HAC_CHECK(strlen(buf) < 6u);
    HAC_CHECK_EQ_U64(text_append_char(buf, 6u, len, 'x'), 6u);
    HAC_CHECK_EQ_U64(text_append_u64(buf, 6u, len, 1u, 1u), 6u);
}

int main(void)
{
    HAC_RUN(fixed_point_rounds_half_away_and_never_writes_negative_zero);
    HAC_RUN(a_number_that_cannot_be_written_is_refused_not_approximated);
    HAC_RUN(signed_integers_cover_the_whole_range);
    HAC_RUN(text_exactly_the_buffer_fits_and_a_byte_less_does_not);
    return hac_test_summary();
}
