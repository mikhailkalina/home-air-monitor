// SPDX-License-Identifier: Apache-2.0
//
// The diagnostics screen lays its text out in fixed columns
// (core/ui/screen_diagnostics.h). This holds it to them, as
// test_screen_home_layout.c does the home screen: it renders the widest
// thing each column can say -- "restarting", "failures 999+", "restarts
// 999+", "Temperature", "warm", "unreliable", a source's name as long as a
// name can be, "Clock: set (--wall-time)", "Recording: no medium" -- with
// every slot filled, and checks by pixels that nothing reaches the margins
// or a rule, that each column keeps clear of the next, that every line stays
// a line of its own, and that the two halves of the title and of the last
// line never meet.

#include <stdint.h>
#include <string.h>

#include "hac_test.h"
#include "ui/framebuffer.h"
#include "ui/screen_diagnostics.h"
#include "ui/view_model.h"

#define PANEL_W 960
#define PANEL_H 540

#define RULE_CLEARANCE 3
#define LINE_CLEARANCE 4
#define HALVES_CLEARANCE 24

static uint8_t g_pixels[PANEL_W / 2 * PANEL_H];
static framebuffer_t g_fb;

static bool inked(int32_t x, int32_t y)
{
    return framebuffer_get_pixel(&g_fb, x, y) != VM_INK_BACKGROUND;
}

static bool clear(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    for (int32_t y = y0; y <= y1; ++y) {
        for (int32_t x = x0; x <= x1; ++x) {
            if (inked(x, y)) {
                return false;
            }
        }
    }
    return true;
}

static int count_bands(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t min_gap)
{
    int bands = 0;
    int32_t empty = min_gap;
    for (int32_t y = y0; y <= y1; ++y) {
        if (clear(x0, y, x1, y)) {
            ++empty;
        } else {
            if (empty >= min_gap) {
                ++bands;
            }
            empty = 0;
        }
    }
    return bands;
}

static int32_t widest_gap(int32_t y0, int32_t y1)
{
    int32_t first = -1;
    int32_t last = -1;
    for (int32_t x = 0; x < PANEL_W; ++x) {
        if (!clear(x, y0, x, y1)) {
            first = (first < 0) ? x : first;
            last = x;
        }
    }
    int32_t widest = 0;
    int32_t run = 0;
    for (int32_t x = first; x >= 0 && x <= last; ++x) {
        run = clear(x, y0, x, y1) ? run + 1 : 0;
        widest = (run > widest) ? run : widest;
    }
    return widest;
}

static void set(char *dst, size_t cap, const char *src)
{
    strncpy(dst, src, cap - 1u);
    dst[cap - 1u] = '\0';
}

// Every slot, every column at its widest.
static diag_view_t widest(const char *clock, const char *recorder)
{
    diag_view_t v;
    memset(&v, 0, sizeof(v));
    v.slot_count = DIAG_MAX_SLOTS;
    for (size_t i = 0u; i < DIAG_MAX_SLOTS; ++i) {
        set(v.slots[i].name, sizeof(v.slots[i].name), "WWWWWWWWWWWWWWWWWWWWWWW");
        v.slots[i].state = "restarting";
        v.slots[i].trouble = true;
        set(v.slots[i].failures, sizeof(v.slots[i].failures), "999+");
        set(v.slots[i].restarts, sizeof(v.slots[i].restarts), "999+");
    }
    static const char *const labels[DIAG_FIELD_COUNT] = {"CO2",      "Temperature", "Humidity",
                                                         "Pressure", "IAQ",         "Gas"};
    for (size_t i = 0u; i < DIAG_FIELD_COUNT; ++i) {
        v.fields[i].label = labels[i];
        v.fields[i].status = "warm";
        v.fields[i].accuracy = "unreliable";
        set(v.fields[i].source, sizeof(v.fields[i].source), "WWWWWWWWWWWWWWWWWWWWWWW");
        v.fields[i].trouble = true;
    }
    v.fields[DIAG_FIELD_CO2].label = "Temperature";  // the widest label on every line
    v.clock_set = true;
    set(v.clock_text, sizeof(v.clock_text), clock);
    set(v.recorder_text, sizeof(v.recorder_text), recorder);
    v.recorder_trouble = true;
    return v;
}

// The column boundaries of one section's lines: the strip before each
// column's left end stays clear.
static void check_columns(const int32_t *xs, size_t n, int32_t y0, int32_t y1, const char *what)
{
    for (size_t i = 1u; i < n; ++i) {
        const bool ok = clear(xs[i] - SCREEN_DIAG_COLUMN_GAP, y0, xs[i] - 1, y1);
        hac_test_check(ok, "a column keeps clear of the next", what, __LINE__);
    }
}

static void check_layout(const char *what)
{
    const int32_t m = SCREEN_DIAG_MARGIN;
    const int32_t rule = SCREEN_DIAG_RULE_PX;

    bool ok = clear(0, 0, m - 1, PANEL_H - 1) && clear(PANEL_W - m, 0, PANEL_W - 1, PANEL_H - 1);
    hac_test_check(ok, "nothing in the left or right margin", what, __LINE__);
    hac_test_check(clear(0, 0, PANEL_W - 1, 5), "nothing in the top margin", what, __LINE__);
    hac_test_check(clear(0, PANEL_H - 10, PANEL_W - 1, PANEL_H - 1), "nothing in the bottom margin",
                   what, __LINE__);

    const int32_t rules[] = {SCREEN_DIAG_TITLE_RULE_Y, SCREEN_DIAG_SLOTS_RULE_Y,
                             SCREEN_DIAG_FIELDS_RULE_Y};
    for (size_t i = 0u; i < sizeof(rules) / sizeof(rules[0]); ++i) {
        ok = clear(m, rules[i] - RULE_CLEARANCE, PANEL_W - m - 1, rules[i] - 1) &&
             clear(m, rules[i] + rule, PANEL_W - m - 1, rules[i] + rule + RULE_CLEARANCE - 1);
        hac_test_check(ok, "text clears the rules", what, __LINE__);
    }

    const int32_t slots_top = SCREEN_DIAG_TITLE_RULE_Y + rule;
    const int32_t slots_bottom = SCREEN_DIAG_SLOTS_RULE_Y - 1;
    const int32_t fields_top = SCREEN_DIAG_SLOTS_RULE_Y + rule;
    const int32_t fields_bottom = SCREEN_DIAG_FIELDS_RULE_Y - 1;

    hac_test_check(count_bands(m, slots_top, PANEL_W - m - 1, slots_bottom, LINE_CLEARANCE) ==
                       (int)DIAG_MAX_SLOTS,
                   "every source's line stays a line of its own", what, __LINE__);
    hac_test_check(count_bands(m, fields_top, PANEL_W - m - 1, fields_bottom, LINE_CLEARANCE) ==
                       (int)DIAG_FIELD_COUNT,
                   "every reading's line stays a line of its own", what, __LINE__);

    const int32_t slot_cols[] = {SCREEN_DIAG_SLOT_NAME_X, SCREEN_DIAG_SLOT_STATE_X,
                                 SCREEN_DIAG_SLOT_FAILURES_X, SCREEN_DIAG_SLOT_RESTARTS_X};
    check_columns(slot_cols, 4u, slots_top, slots_bottom, what);
    const int32_t field_cols[] = {SCREEN_DIAG_FIELD_LABEL_X, SCREEN_DIAG_FIELD_STATUS_X,
                                  SCREEN_DIAG_FIELD_ACCURACY_X, SCREEN_DIAG_FIELD_SOURCE_X};
    check_columns(field_cols, 4u, fields_top, fields_bottom, what);

    hac_test_check(widest_gap(0, SCREEN_DIAG_TITLE_RULE_Y - 1) >= HALVES_CLEARANCE,
                   "the title's halves stay apart", what, __LINE__);
    hac_test_check(widest_gap(SCREEN_DIAG_FIELDS_RULE_Y + rule, PANEL_H - 1) >= HALVES_CLEARANCE,
                   "the last line's halves stay apart", what, __LINE__);
}

static void the_widest_texts_fit(void)
{
    const char *clocks[] = {"Clock: set (--wall-time)", "Clock: set (recording)", "Clock: not set"};
    const char *recorders[] = {"Recording: no medium", "Recording: waiting", "Recording: off"};
    for (size_t i = 0u; i < 3u; ++i) {
        const diag_view_t v = widest(clocks[i], recorders[i]);
        screen_diagnostics_render(&g_fb, &v);
        check_layout(clocks[i]);
    }
}

static void a_long_name_is_shortened_to_its_column(void)
{
    diag_view_t v = widest("Clock: set", "Recording: on");
    screen_diagnostics_render(&g_fb, &v);
    // The name's column ends a gap before the state's.
    const int32_t top = SCREEN_DIAG_TITLE_RULE_Y + SCREEN_DIAG_RULE_PX;
    HAC_CHECK(clear(SCREEN_DIAG_SLOT_STATE_X - SCREEN_DIAG_COLUMN_GAP, top,
                    SCREEN_DIAG_SLOT_STATE_X - 1, SCREEN_DIAG_SLOTS_RULE_Y - 1));
    // And a source's name runs no further than the margin.
    HAC_CHECK(clear(PANEL_W - SCREEN_DIAG_MARGIN, 0, PANEL_W - 1, PANEL_H - 1));
}

static void no_sources_says_so(void)
{
    diag_view_t v = widest("Clock: set", "Recording: on");
    v.slot_count = 0u;
    screen_diagnostics_render(&g_fb, &v);
    const int32_t top = SCREEN_DIAG_TITLE_RULE_Y + SCREEN_DIAG_RULE_PX;
    HAC_CHECK(count_bands(SCREEN_DIAG_MARGIN, top, PANEL_W - SCREEN_DIAG_MARGIN - 1,
                          SCREEN_DIAG_SLOTS_RULE_Y - 1, LINE_CLEARANCE) == 1);
}

int main(void)
{
    framebuffer_init(&g_fb, g_pixels, sizeof(g_pixels), PANEL_W, PANEL_H, PIXFMT_GRAY4);
    HAC_RUN(the_widest_texts_fit);
    HAC_RUN(a_long_name_is_shortened_to_its_column);
    HAC_RUN(no_sources_says_so);
    return hac_test_summary();
}
