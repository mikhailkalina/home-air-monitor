// SPDX-License-Identifier: Apache-2.0

#include "recording/record_text.h"

#include <math.h>

// The largest magnitude record_text_fixed() scales to an integer: past it a
// double no longer holds every integer exactly, and no reading comes near
// it (a gas resistance of 10^8 ohm with no decimals is 10^8).
#define RECORD_TEXT_MAX_SCALED 1e15

// Ten decimals is more than any column writes; a bound for the scale loop.
#define RECORD_TEXT_MAX_DECIMALS 10u

#define RECORD_TEXT_U64_DIGITS 20u

size_t record_text_char(char *dst, size_t cap, size_t len, char c)
{
    if (len + 1u >= cap) {
        return cap;  // no room for the character and the terminator
    }
    dst[len++] = c;
    dst[len] = '\0';
    return len;
}

size_t record_text_str(char *dst, size_t cap, size_t len, const char *src)
{
    for (; *src != '\0'; ++src) {
        len = record_text_char(dst, cap, len, *src);
        if (len >= cap) {
            return cap;
        }
    }
    return len;
}

size_t record_text_u64(char *dst, size_t cap, size_t len, uint64_t value, unsigned width)
{
    char digits[RECORD_TEXT_U64_DIGITS];
    unsigned n = 0u;
    do {
        digits[n++] = (char)('0' + (int)(value % 10u));
        value /= 10u;
    } while (value != 0u && n < RECORD_TEXT_U64_DIGITS);
    while (n < width && n < RECORD_TEXT_U64_DIGITS) {
        digits[n++] = '0';
    }
    while (n > 0u) {
        len = record_text_char(dst, cap, len, digits[--n]);
        if (len >= cap) {
            return cap;
        }
    }
    return len;
}

size_t record_text_fixed(char *dst, size_t cap, size_t len, double value, unsigned decimals)
{
    if (!isfinite(value) || decimals > RECORD_TEXT_MAX_DECIMALS) {
        return cap;
    }
    double scale = 1.0;
    for (unsigned i = 0u; i < decimals; ++i) {
        scale *= 10.0;
    }
    const double scaled = round(fabs(value) * scale);
    if (scaled > RECORD_TEXT_MAX_SCALED) {
        return cap;
    }
    const uint64_t magnitude = (uint64_t)scaled;
    uint64_t divisor = 1u;
    for (unsigned i = 0u; i < decimals; ++i) {
        divisor *= 10u;
    }

    // "-0.00" would be a negative zero: written as "0.00".
    if (value < 0.0 && magnitude != 0u) {
        len = record_text_char(dst, cap, len, '-');
    }
    len = record_text_u64(dst, cap, len, magnitude / divisor, 1u);
    if (decimals > 0u) {
        len = record_text_char(dst, cap, len, '.');
        len = record_text_u64(dst, cap, len, magnitude % divisor, decimals);
    }
    return len;
}
