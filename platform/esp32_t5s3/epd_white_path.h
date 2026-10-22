// SPDX-License-Identifier: Apache-2.0
//
// The partial refresh's waveform mode, "through white", built at boot from
// two of the modes EPDiy ships for this panel and kept free of EPDiy's headers
// so the host build can test it (tests/unit/test_epd_white_path.c), as
// adp_net_pure.c is (docs/adr/0035-the-vendors-full-refresh.md, second
// amendment).
//
// Why: since ADR 0035 a full refresh draws every grey darkened from white
// (EPDiy's MODE_EPDIY_WHITE_TO_GL16, the vendor's fifteen darkening frames).
// MODE_GL16, the partial refresh until then, takes a pixel from A to B
// through black -- darkened to black, then lightened to B -- and, worse, does
// the same to every unchanged grey L in each row it writes and each column of
// EPDiy's global mask (its L -> L entries are not empty). Both paths leave
// the same level a different grey, and on the board the static greys beside a
// changed digit went lighter in patches, with either full-refresh draw.
//
// This mode takes a pixel from A to B through white instead:
//
//   * A == B: nothing, ever -- an unchanged pixel is not driven at all;
//   * then, in GL16's own lightening phases (those GL16 drives a black pixel
//     to white with, their times with them), every pixel that changes and is
//     not white already is driven towards white -- all of them, from any A:
//     white saturates, and it is the state the darkening below starts from;
//   * then, in WHITE_TO_GL16's phases and times, every pixel that changes to
//     B < 15 is darkened exactly as WHITE_TO_GL16 darkens white to B.
//
// So a grey drawn by a partial refresh is the same grey a full refresh draws,
// and nothing a partial refresh does not change is touched. A pixel turning
// white is only lightened; a white one darkening is only darkened.
//
// The tables use EPDiy's layout (src/output_common/lut.c,
// build_1ppB_lut_64k()): per phase, 16 rows by target level, 4 bytes each, a
// 2-bit operation per source level, the highest bits for the lowest level of
// the four.

#ifndef HAC_PLATFORM_ESP32_T5S3_EPD_WHITE_PATH_H
#define HAC_PLATFORM_ESP32_T5S3_EPD_WHITE_PATH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_WHITE_PATH_LEVELS 16u
#define EPD_WHITE_PATH_WHITE 15u
// Bytes of one phase's table: 16 targets x 16 sources x 2 bits.
#define EPD_WHITE_PATH_PHASE_BYTES 64u
// Room for the mode built: GL16's lightening phases and WHITE_TO_GL16's, 15
// each in EPDiy's waveform for this panel.
#define EPD_WHITE_PATH_MAX_PHASES 32u

// EPDiy's operations on a pixel in one phase.
#define EPD_WHITE_PATH_OP_NONE 0u
#define EPD_WHITE_PATH_OP_DARKEN 1u
#define EPD_WHITE_PATH_OP_LIGHTEN 2u

// One of EPDiy's modes, as its EpdWaveformPhases holds it.
typedef struct {
    size_t phases;
    const uint8_t *luts;  // phases x EPD_WHITE_PATH_PHASE_BYTES
    const int *times;     // phases, the drive per row in tenths of a microsecond
} epd_white_path_mode_t;

// The mode built: `luts` and `times` hold `phases` phases.
typedef struct {
    size_t phases;
    uint8_t luts[EPD_WHITE_PATH_MAX_PHASES * EPD_WHITE_PATH_PHASE_BYTES];
    int times[EPD_WHITE_PATH_MAX_PHASES];
} epd_white_path_t;

// The operation `luts`' phase `phase` applies to a pixel going from `from` to
// `to`.
uint8_t epd_white_path_op(const uint8_t *luts, size_t phase, uint8_t from, uint8_t to);

// Builds the mode from GL16 (`gl16`: its lightening phases are those that
// drive black to white) and WHITE_TO_GL16 (`white_to`). Returns 0, or -1 when
// the two would not fit EPD_WHITE_PATH_MAX_PHASES, when GL16 has no
// lightening phase, or an argument is NULL -- `out` is then empty.
int epd_white_path_build(const epd_white_path_mode_t *gl16, const epd_white_path_mode_t *white_to,
                         epd_white_path_t *out);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_EPD_WHITE_PATH_H
