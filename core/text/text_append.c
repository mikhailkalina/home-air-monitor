// SPDX-License-Identifier: Apache-2.0

#include "text/text_append.h"

#include <math.h>

// The largest magnitude text_append_fixed() scales to an integer: past it a
// double no longer holds every integer exactly, and no reading comes near
// it (a gas resistance of 10^8 ohm with no decimals is 10^8).
#define TEXT_MAX_SCALED 1e15

// Ten decimals is more than any column or payload writes; a bound for the
// scale loop.
#define TEXT_MAX_DECIMALS 10u

#define TEXT_U64_DIGITS 20u

size_t text_append_char(char *dst, size_t cap, size_t len, char c)
{
    if (len + 1u >= cap) {
        return cap;  // no room for the character and the terminator
    }
    dst[len++] = c;
    dst[len] = '\0';
    return len;
}

size_t text_append_str(char *dst, size_t cap, size_t len, const char *src)
{
    for (; *src != '\0'; ++src) {
        len = text_append_char(dst, cap, len, *src);
        if (len >= cap) {
            return cap;
        }
    }
    return len;
}

size_t text_append_u64(char *dst, size_t cap, size_t len, uint64_t value, unsigned width)
{
    char digits[TEXT_U64_DIGITS];
    unsigned n = 0u;
    do {
        digits[n++] = (char)('0' + (int)(value % 10u));
        value /= 10u;
    } while (value != 0u && n < TEXT_U64_DIGITS);
    while (n < width && n < TEXT_U64_DIGITS) {
        digits[n++] = '0';
    }
    while (n > 0u) {
        len = text_append_char(dst, cap, len, digits[--n]);
        if (len >= cap) {
            return cap;
        }
    }
    return len;
}

size_t text_append_i64(char *dst, size_t cap, size_t len, int64_t value)
{
    if (value >= 0) {
        return text_append_u64(dst, cap, len, (uint64_t)value, 1u);
    }
    // -(value + 1) + 1, so that INT64_MIN never overflows on the way.
    const uint64_t magnitude = (uint64_t)(-(value + 1)) + 1u;
    len = text_append_char(dst, cap, len, '-');
    return text_append_u64(dst, cap, len, magnitude, 1u);
}

static double scale_of(unsigned decimals)
{
    double scale = 1.0;
    for (unsigned i = 0u; i < decimals; ++i) {
        scale *= 10.0;
    }
    return scale;
}

bool text_fixed_representable(double value, unsigned decimals)
{
    return isfinite(value) && decimals <= TEXT_MAX_DECIMALS &&
           round(fabs(value) * scale_of(decimals)) <= TEXT_MAX_SCALED;
}

size_t text_append_fixed(char *dst, size_t cap, size_t len, double value, unsigned decimals)
{
    if (!text_fixed_representable(value, decimals)) {
        return cap;
    }
    const uint64_t magnitude = (uint64_t)round(fabs(value) * scale_of(decimals));
    uint64_t divisor = 1u;
    for (unsigned i = 0u; i < decimals; ++i) {
        divisor *= 10u;
    }

    // "-0.00" would be a negative zero: written as "0.00".
    if (value < 0.0 && magnitude != 0u) {
        len = text_append_char(dst, cap, len, '-');
    }
    len = text_append_u64(dst, cap, len, magnitude / divisor, 1u);
    if (decimals > 0u) {
        len = text_append_char(dst, cap, len, '.');
        len = text_append_u64(dst, cap, len, magnitude % divisor, decimals);
    }
    return len;
}
