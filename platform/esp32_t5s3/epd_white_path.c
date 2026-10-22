// SPDX-License-Identifier: Apache-2.0

#include "epd_white_path.h"

#include <string.h>

// The 2-bit field of `from` in its byte: the lowest level of four in the
// highest bits.
static unsigned shift_of(uint8_t from)
{
    return 6u - 2u * (unsigned)(from % 4u);
}

static size_t byte_of(size_t phase, uint8_t from, uint8_t to)
{
    return phase * EPD_WHITE_PATH_PHASE_BYTES + (size_t)to * 4u + (size_t)(from / 4u);
}

uint8_t epd_white_path_op(const uint8_t *luts, size_t phase, uint8_t from, uint8_t to)
{
    return (uint8_t)(((unsigned)luts[byte_of(phase, from, to)] >> shift_of(from)) & 3u);
}

static void set_op(uint8_t *luts, size_t phase, uint8_t from, uint8_t to, uint8_t op)
{
    const size_t i = byte_of(phase, from, to);
    luts[i] = (uint8_t)((unsigned)luts[i] | ((unsigned)op << shift_of(from)));
}

int epd_white_path_build(const epd_white_path_mode_t *gl16, const epd_white_path_mode_t *white_to,
                         epd_white_path_t *out)
{
    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (gl16 == NULL || white_to == NULL || gl16->luts == NULL || gl16->times == NULL ||
        white_to->luts == NULL || white_to->times == NULL) {
        return -1;
    }

    // GL16's lightening phases: those in which it drives black towards white.
    size_t lighten[EPD_WHITE_PATH_MAX_PHASES];
    size_t n_lighten = 0u;
    for (size_t p = 0u; p < gl16->phases; ++p) {
        if (epd_white_path_op(gl16->luts, p, 0u, EPD_WHITE_PATH_WHITE) ==
            EPD_WHITE_PATH_OP_LIGHTEN) {
            if (n_lighten == EPD_WHITE_PATH_MAX_PHASES) {
                return -1;
            }
            lighten[n_lighten++] = p;
        }
    }
    if (n_lighten == 0u || n_lighten + white_to->phases > EPD_WHITE_PATH_MAX_PHASES) {
        return -1;
    }

    // Towards white: every pixel that changes and is not white already.
    for (size_t i = 0u; i < n_lighten; ++i) {
        out->times[i] = gl16->times[lighten[i]];
        for (uint8_t from = 0u; from < EPD_WHITE_PATH_WHITE; ++from) {
            for (uint8_t to = 0u; to < EPD_WHITE_PATH_LEVELS; ++to) {
                if (from != to) {
                    set_op(out->luts, i, from, to, EPD_WHITE_PATH_OP_LIGHTEN);
                }
            }
        }
    }

    // Then darkened from white as WHITE_TO_GL16 darkens white to the target.
    for (size_t k = 0u; k < white_to->phases; ++k) {
        const size_t phase = n_lighten + k;
        out->times[phase] = white_to->times[k];
        for (uint8_t to = 0u; to < EPD_WHITE_PATH_WHITE; ++to) {
            if (epd_white_path_op(white_to->luts, k, EPD_WHITE_PATH_WHITE, to) !=
                EPD_WHITE_PATH_OP_DARKEN) {
                continue;
            }
            for (uint8_t from = 0u; from < EPD_WHITE_PATH_LEVELS; ++from) {
                if (from != to) {
                    set_op(out->luts, phase, from, to, EPD_WHITE_PATH_OP_DARKEN);
                }
            }
        }
    }

    out->phases = n_lighten + white_to->phases;
    return 0;
}
