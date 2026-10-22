// SPDX-License-Identifier: Apache-2.0
//
// The partial refresh's mode "through white" (platform/esp32_t5s3/
// epd_white_path.h), built from tables shaped as EPDiy's GL16 and
// WHITE_TO_GL16 are for this panel: GL16 takes A to B through black -- the
// last A of its 15 darkening phases, then the first B of its 15 lightening
// ones, and drives an unchanged grey down and back too; WHITE_TO_GL16 darkens
// white to B in its first 15 - B phases. EPDiy's own tables are not here --
// its waveform is a third-party header the host build does not have -- so
// these are written from the rules read out of it
// (docs/adr/0035-the-vendors-full-refresh.md, second amendment).

#include <string.h>

#include "epd_white_path.h"
#include "hac_test.h"

#define STEPS 15u

static uint8_t g_gl16_luts[2u * STEPS * EPD_WHITE_PATH_PHASE_BYTES];
static int g_gl16_times[2u * STEPS];
static uint8_t g_white_to_luts[STEPS * EPD_WHITE_PATH_PHASE_BYTES];
static int g_white_to_times[STEPS];

static void put(uint8_t *luts, size_t phase, uint8_t from, uint8_t to, uint8_t op)
{
    const size_t i = phase * EPD_WHITE_PATH_PHASE_BYTES + (size_t)to * 4u + (size_t)(from / 4u);
    luts[i] |= (uint8_t)(op << (6u - 2u * (unsigned)(from % 4u)));
}

static void make_tables(epd_white_path_mode_t *gl16, epd_white_path_mode_t *white_to)
{
    memset(g_gl16_luts, 0, sizeof(g_gl16_luts));
    memset(g_white_to_luts, 0, sizeof(g_white_to_luts));
    for (size_t p = 0u; p < STEPS; ++p) {
        g_gl16_times[p] = 30 + (int)p;          // darkening phases
        g_gl16_times[STEPS + p] = 10 + (int)p;  // lightening phases
        g_white_to_times[p] = 100 + (int)p;
    }
    for (uint8_t from = 0u; from < 16u; ++from) {
        for (uint8_t to = 0u; to < 16u; ++to) {
            if ((from == to && (from == 0u || from == 15u))) {
                continue;
            }
            for (size_t p = 0u; p < STEPS; ++p) {
                if (p + from >= STEPS) {
                    put(g_gl16_luts, p, from, to, EPD_WHITE_PATH_OP_DARKEN);
                }
                if (p < to) {
                    put(g_gl16_luts, STEPS + p, from, to, EPD_WHITE_PATH_OP_LIGHTEN);
                }
            }
        }
    }
    for (uint8_t to = 0u; to < 15u; ++to) {
        for (size_t k = 0u; k + to < STEPS; ++k) {
            put(g_white_to_luts, k, 15u, to, EPD_WHITE_PATH_OP_DARKEN);
        }
    }
    gl16->phases = 2u * STEPS;
    gl16->luts = g_gl16_luts;
    gl16->times = g_gl16_times;
    white_to->phases = STEPS;
    white_to->luts = g_white_to_luts;
    white_to->times = g_white_to_times;
}

static epd_white_path_t g_mode;

static void built(void)
{
    epd_white_path_mode_t gl16;
    epd_white_path_mode_t white_to;
    make_tables(&gl16, &white_to);
    HAC_CHECK_EQ_INT(epd_white_path_build(&gl16, &white_to, &g_mode), 0);
}

// GL16's 15 lightening phases, then WHITE_TO_GL16's 15, each with its time.
static void the_phases_are_gl16s_lightening_then_white_to_gl16s(void)
{
    built();
    HAC_CHECK_EQ_INT((int)g_mode.phases, 30);
    for (size_t p = 0u; p < STEPS; ++p) {
        HAC_CHECK_EQ_INT(g_mode.times[p], g_gl16_times[STEPS + p]);
        HAC_CHECK_EQ_INT(g_mode.times[STEPS + p], g_white_to_times[p]);
    }
}

// The point of it: an unchanged pixel is never driven, whatever its level --
// GL16 drives an unchanged grey down and back.
static void an_unchanged_pixel_is_never_driven(void)
{
    built();
    for (uint8_t level = 0u; level < 16u; ++level) {
        for (size_t p = 0u; p < g_mode.phases; ++p) {
            hac_test_check(epd_white_path_op(g_mode.luts, p, level, level) ==
                               EPD_WHITE_PATH_OP_NONE,
                           "an unchanged pixel driven", __FILE__, __LINE__);
        }
    }
    HAC_CHECK(epd_white_path_op(g_gl16_luts, 14u, 4u, 4u) != EPD_WHITE_PATH_OP_NONE);
}

// A pixel that changes: to white in every lightening phase unless white
// already, then darkened exactly as WHITE_TO_GL16 darkens white to its target.
static void a_change_goes_through_white_to_the_full_refreshs_grey(void)
{
    built();
    for (uint8_t from = 0u; from < 16u; ++from) {
        for (uint8_t to = 0u; to < 16u; ++to) {
            if (from == to) {
                continue;
            }
            for (size_t p = 0u; p < STEPS; ++p) {
                const uint8_t want =
                    (from == 15u) ? EPD_WHITE_PATH_OP_NONE : EPD_WHITE_PATH_OP_LIGHTEN;
                hac_test_check(epd_white_path_op(g_mode.luts, p, from, to) == want,
                               "the lightening phase", __FILE__, __LINE__);
                const uint8_t dark = epd_white_path_op(g_white_to_luts, p, 15u, to);
                hac_test_check(epd_white_path_op(g_mode.luts, STEPS + p, from, to) == dark,
                               "the darkening phase", __FILE__, __LINE__);
            }
        }
    }
    // Spot checks: white to black is WHITE_TO_GL16's 15 darkening phases and
    // nothing else; black to white the 15 lightening ones.
    HAC_CHECK_EQ_INT(epd_white_path_op(g_mode.luts, 0u, 15u, 0u), EPD_WHITE_PATH_OP_NONE);
    HAC_CHECK_EQ_INT(epd_white_path_op(g_mode.luts, STEPS + 14u, 15u, 0u),
                     EPD_WHITE_PATH_OP_DARKEN);
    HAC_CHECK_EQ_INT(epd_white_path_op(g_mode.luts, 14u, 0u, 15u), EPD_WHITE_PATH_OP_LIGHTEN);
    HAC_CHECK_EQ_INT(epd_white_path_op(g_mode.luts, STEPS, 0u, 15u), EPD_WHITE_PATH_OP_NONE);
    // Grey 4 to grey 9: whitened, then the first 6 darkening phases only.
    HAC_CHECK_EQ_INT(epd_white_path_op(g_mode.luts, STEPS + 5u, 4u, 9u), EPD_WHITE_PATH_OP_DARKEN);
    HAC_CHECK_EQ_INT(epd_white_path_op(g_mode.luts, STEPS + 6u, 4u, 9u), EPD_WHITE_PATH_OP_NONE);
}

static void what_cannot_be_built_is_refused_and_left_empty(void)
{
    epd_white_path_mode_t gl16;
    epd_white_path_mode_t white_to;
    make_tables(&gl16, &white_to);
    HAC_CHECK_EQ_INT(epd_white_path_build(NULL, &white_to, &g_mode), -1);
    HAC_CHECK_EQ_INT((int)g_mode.phases, 0);
    HAC_CHECK_EQ_INT(epd_white_path_build(&gl16, &white_to, NULL), -1);

    // Too many phases for the room there is.
    white_to.phases = EPD_WHITE_PATH_MAX_PHASES;
    HAC_CHECK_EQ_INT(epd_white_path_build(&gl16, &white_to, &g_mode), -1);
    HAC_CHECK_EQ_INT((int)g_mode.phases, 0);

    // A "GL16" that never lightens black: nothing to go through white with.
    make_tables(&gl16, &white_to);
    gl16.phases = STEPS;  // its darkening half only
    HAC_CHECK_EQ_INT(epd_white_path_build(&gl16, &white_to, &g_mode), -1);
}

int main(void)
{
    HAC_RUN(the_phases_are_gl16s_lightening_then_white_to_gl16s);
    HAC_RUN(an_unchanged_pixel_is_never_driven);
    HAC_RUN(a_change_goes_through_white_to_the_full_refreshs_grey);
    HAC_RUN(what_cannot_be_built_is_refused_and_left_empty);
    return hac_test_summary();
}
