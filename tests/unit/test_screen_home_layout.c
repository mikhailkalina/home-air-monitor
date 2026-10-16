// SPDX-License-Identifier: Apache-2.0
//
// The home screen sets every text as large as the widest thing it can say
// allows (screen_home.h). This holds it to that: it renders the widest and
// deepest strings each field can show -- "40000" ppm, "-40.0" C, "1100" hPa,
// "warm", "ERR", "n/a", an IAQ's widest bands beside the question mark, the
// error box and the longest "... not calibrated" in the footer, beside the
// widest "updated HH:MM" and "updated DD.MM HH:MM" this font draws -- on the
// 960x540 panel,
// and checks by pixels that nothing reaches the margins, the band, a grid
// separator or a rule, and that the left and right halves of the status bar
// and the footer never meet. A larger font, a longer label or a moved
// baseline that would make two things touch fails here before it reaches the
// glass.
//
// The golden tests (tests/golden/) show what the screen looks like; this one
// says it still fits when the numbers are at their widest.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "domain/civil_time.h"
#include "domain/measurement.h"
#include "hac_test.h"
#include "ui/fonts/font_dejavu_sans.h"
#include "ui/framebuffer.h"
#include "ui/gfx.h"
#include "ui/screen_home.h"
#include "ui/view_model.h"

#define PANEL_W 960
#define PANEL_H 540

#define RENDER_TS_MS 100000000u
#define SOURCE_ID 1u

// Read with RENDER_TS_MS: 2026-10-16T11:07:42Z. Every reading here carries
// it, so the footer shows a time of day unless a case says otherwise.
#define RENDER_WALL_MS 1792148862000u

// What a text must keep clear of: the band beside the CO2 reading, a rule
// above or below it, the halves of a line from each other.
#define HEAD_CLEARANCE 14
#define RULE_CLEARANCE 3
#define HALVES_CLEARANCE 24
#define LINE_CLEARANCE 4

static uint8_t *g_pixels;
static framebuffer_t g_fb;
static time_zone_t g_zone;  // Vilnius, set in main()

// The "updated" text to draw in place of the one the reading gives, or NULL:
// how the widest time strings are put on the screen.
static const char *g_updated_text;

static void build(view_model_t *vm, const air_reading_t *r)
{
    view_model_build(vm, r, RENDER_TS_MS, &g_zone);
    if (g_updated_text != NULL) {
        strncpy(vm->updated_text, g_updated_text, sizeof(vm->updated_text) - 1u);
        vm->updated_text[sizeof(vm->updated_text) - 1u] = '\0';
        vm->has_updated = true;
    }
}

static void render(const air_reading_t *r)
{
    view_model_t vm;
    build(&vm, r);
    screen_home_render(&g_fb, &vm);
}

static bool inked(int32_t x, int32_t y)
{
    return framebuffer_get_pixel(&g_fb, x, y) != VM_INK_BACKGROUND;
}

// True when no pixel of the rectangle carries ink.
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

// The widest run of empty columns between the first and the last inked
// column of rows y0..y1: how far apart the left and right halves of a line
// are.
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

// How many bands of inked rows the rectangle holds, apart by at least
// `min_gap` empty rows: lines of text that stay clear of each other count
// separately, lines that touch merge into one.
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

static void check_layout(const char *what)
{
    const int32_t m = SCREEN_HOME_MARGIN;
    const int32_t rule = SCREEN_HOME_RULE_PX;
    bool ok = true;

    // The margins, all round.
    ok = clear(0, 0, m - 1, PANEL_H - 1) && clear(PANEL_W - m, 0, PANEL_W - 1, PANEL_H - 1);
    hac_test_check(ok, "nothing in the left or right margin", what, __LINE__);
    hac_test_check(clear(0, 0, PANEL_W - 1, 5), "nothing in the top margin", what, __LINE__);
    hac_test_check(clear(0, PANEL_H - 10, PANEL_W - 1, PANEL_H - 1), "nothing in the bottom margin",
                   what, __LINE__);

    // Text clears every rule above and below.
    const int32_t rules[] = {SCREEN_HOME_BAR_RULE_Y, SCREEN_HOME_HEAD_RULE_Y,
                             SCREEN_HOME_FOOT_RULE_Y};
    for (size_t i = 0u; i < sizeof(rules) / sizeof(rules[0]); ++i) {
        ok = clear(m, rules[i] - RULE_CLEARANCE, PANEL_W - m - 1, rules[i] - 1) &&
             clear(m, rules[i] + rule, PANEL_W - m - 1, rules[i] + rule + RULE_CLEARANCE - 1);
        hac_test_check(ok, "text clears the rules", what, __LINE__);
    }

    // The CO2 reading, its unit and mark clear the band.
    const int32_t banner_x = PANEL_W - m - SCREEN_HOME_BANNER_W;
    ok = clear(banner_x - HEAD_CLEARANCE, SCREEN_HOME_BAR_RULE_Y + rule, banner_x - 1,
               SCREEN_HOME_HEAD_RULE_Y - 1);
    hac_test_check(ok, "the CO2 reading clears the band", what, __LINE__);

    // Each cell's text keeps off the separators on both sides.
    const int32_t column_w = (PANEL_W - 2 * m) / SCREEN_HOME_GRID_COLUMNS;
    for (int32_t k = 1; k < SCREEN_HOME_GRID_COLUMNS; ++k) {
        const int32_t sep = m + k * column_w;
        const int32_t sep_left = sep - rule / 2;
        const int32_t sep_right = sep_left + rule - 1;
        ok = clear(sep - SCREEN_HOME_CELL_GAP, SCREEN_HOME_HEAD_RULE_Y + rule, sep_left - 1,
                   SCREEN_HOME_FOOT_RULE_Y - 1) &&
             clear(sep_right + 1, SCREEN_HOME_HEAD_RULE_Y + rule, sep + SCREEN_HOME_CELL_PAD - 1,
                   SCREEN_HOME_FOOT_RULE_Y - 1);
        hac_test_check(ok, "grid text keeps off the separators", what, __LINE__);
    }

    // Within a cell: the label, the number and the unit line stay three
    // lines. Within the headline: the label stays clear of the number.
    for (int32_t k = 0; k < SCREEN_HOME_GRID_COLUMNS; ++k) {
        const int32_t x0 = m + k * column_w + rule;
        const int32_t x1 = m + (k + 1) * column_w - rule;
        hac_test_check(count_bands(x0, SCREEN_HOME_HEAD_RULE_Y + rule, x1,
                                   SCREEN_HOME_FOOT_RULE_Y - 1, LINE_CLEARANCE) == 3,
                       "a cell's label, number and unit stay apart", what, __LINE__);
    }
    hac_test_check(count_bands(m, SCREEN_HOME_BAR_RULE_Y + rule, banner_x - 1,
                               SCREEN_HOME_HEAD_RULE_Y - 1, LINE_CLEARANCE) == 2,
                   "the CO2 label stays clear of the reading", what, __LINE__);

    // The two halves of the status bar and of the footer stay apart.
    hac_test_check(widest_gap(0, SCREEN_HOME_BAR_RULE_Y - 1) >= HALVES_CLEARANCE,
                   "the status bar's halves stay apart", what, __LINE__);
    hac_test_check(widest_gap(SCREEN_HOME_FOOT_RULE_Y + rule, PANEL_H - 1) >= HALVES_CLEARANCE,
                   "the footer's halves stay apart", what, __LINE__);
}

// The widest number each field can show, with the battery full: the
// longest status bar.
static air_reading_t widest_numbers(void)
{
    air_reading_t r;
    air_reading_init(&r);
    r.co2_ppm = measurement_ok(40000.0f, RENDER_TS_MS, SOURCE_ID);      // the SCD4x's range
    r.temperature_c = measurement_ok(-40.0f, RENDER_TS_MS, SOURCE_ID);  // the BME68x's range
    r.humidity_rh = measurement_ok(100.0f, RENDER_TS_MS, SOURCE_ID);
    r.pressure_hpa = measurement_ok(1100.0f, RENDER_TS_MS, SOURCE_ID);  // the BME68x's range
    r.gas_resistance_ohm = measurement_ok(9999000.0f, RENDER_TS_MS, SOURCE_ID);  // "9999" kOhm
    r.battery_percent = measurement_ok(100.0f, RENDER_TS_MS, SOURCE_ID);
    r.net_connected = true;
    r.wall_time_ms = RENDER_WALL_MS;
    return r;
}

static void the_widest_numbers_fit(void)
{
    air_reading_t r = widest_numbers();
    render(&r);
    check_layout("widest numbers");
}

static void every_field_warming_up_fits(void)
{
    air_reading_t r;
    air_reading_init(&r);
    r.co2_ppm = measurement_status(VAL_WARMUP, RENDER_TS_MS, SOURCE_ID);
    r.temperature_c = measurement_status(VAL_WARMUP, RENDER_TS_MS, SOURCE_ID);
    r.humidity_rh = measurement_status(VAL_WARMUP, RENDER_TS_MS, SOURCE_ID);
    r.pressure_hpa = measurement_status(VAL_WARMUP, RENDER_TS_MS, SOURCE_ID);
    r.gas_resistance_ohm = measurement_status(VAL_WARMUP, RENDER_TS_MS, SOURCE_ID);
    r.battery_percent = measurement_status(VAL_WARMUP, RENDER_TS_MS, SOURCE_ID);
    render(&r);
    check_layout("every field warming up");  // "warm", the widest placeholder
}

// Every field failed: "ERR" everywhere, the error marks, and the knocked-out
// box around "6 sensor errors" in the footer.
static void every_field_failed_fits(void)
{
    air_reading_t r;
    air_reading_init(&r);
    r.co2_ppm = measurement_status(VAL_ERROR, RENDER_TS_MS, SOURCE_ID);
    r.temperature_c = measurement_status(VAL_ERROR, RENDER_TS_MS, SOURCE_ID);
    r.humidity_rh = measurement_status(VAL_ERROR, RENDER_TS_MS, SOURCE_ID);
    r.pressure_hpa = measurement_status(VAL_ERROR, RENDER_TS_MS, SOURCE_ID);
    r.gas_resistance_ohm = measurement_status(VAL_ERROR, RENDER_TS_MS, SOURCE_ID);
    r.battery_percent = measurement_status(VAL_ERROR, RENDER_TS_MS, SOURCE_ID);
    render(&r);
    check_layout("every field failed");
}

// Nothing provided: "n/a", whose slash reaches furthest below the baseline.
static void nothing_provided_fits(void)
{
    air_reading_t r;
    air_reading_init(&r);
    render(&r);
    check_layout("nothing provided");
}

// The widest numbers gone stale since the evening before: the clock marks
// beside them, and the footer's dated form.
static void the_widest_numbers_gone_stale_fit(void)
{
    air_reading_t r = widest_numbers();
    const uint64_t measured_ms = RENDER_TS_MS - 15u * 3600000u;  // 23:07:42 the day before
    r.co2_ppm.ts_ms = measured_ms;
    r.temperature_c.ts_ms = measured_ms;
    r.humidity_rh.ts_ms = measured_ms;
    r.pressure_hpa.ts_ms = measured_ms;
    r.gas_resistance_ohm.ts_ms = measured_ms;
    r.co2_ppm.status = VAL_STALE;
    r.temperature_c.status = VAL_STALE;
    r.humidity_rh.status = VAL_STALE;
    r.pressure_hpa.status = VAL_STALE;
    r.gas_resistance_ohm.status = VAL_STALE;
    render(&r);
    check_layout("widest numbers, stale");

    view_model_t vm;
    build(&vm, &r);
    HAC_CHECK(strcmp(vm.updated_text, "15.10 23:07") == 0);
}

// The widest text of the "updated" line, measured over every string it can
// say: every HH:MM of a day, and every DD.MM HH:MM of a leap year. Digits
// differ in their side bearings even where they share an advance, so the
// ink decides.
static void widest_updated_text(char *out, size_t cap, bool dated)
{
    const font_t *font = &font_dejavu_sans_40;
    int32_t widest = -1;
    char text[VM_UPDATED_TEXT_MAX];
    for (uint8_t month = dated ? 1u : 12u; month <= 12u; ++month) {
        const uint8_t last_day = dated ? civil_days_in_month(2028, month) : 1u;
        for (uint8_t day = 1u; day <= last_day; ++day) {
            for (uint8_t hour = 0u; hour < 24u; ++hour) {
                for (uint8_t minute = 0u; minute < 60u; ++minute) {
                    char *p = text;
                    if (dated) {
                        *p++ = (char)('0' + day / 10u);
                        *p++ = (char)('0' + day % 10u);
                        *p++ = '.';
                        *p++ = (char)('0' + month / 10u);
                        *p++ = (char)('0' + month % 10u);
                        *p++ = ' ';
                    }
                    *p++ = (char)('0' + hour / 10u);
                    *p++ = (char)('0' + hour % 10u);
                    *p++ = ':';
                    *p++ = (char)('0' + minute / 10u);
                    *p++ = (char)('0' + minute % 10u);
                    *p = '\0';
                    const int32_t w = gfx_measure_text_ink(font, text);
                    if (w > widest) {
                        widest = w;
                        strncpy(out, text, cap - 1u);
                        out[cap - 1u] = '\0';
                    }
                }
            }
        }
    }
}

// The widest time, and the widest dated one, beside the footer's widest
// texts on the right: the error box and the longest calibration note.
static void the_widest_update_times_fit(void)
{
    char hhmm[VM_UPDATED_TEXT_MAX];
    char dated[VM_UPDATED_TEXT_MAX];
    widest_updated_text(hhmm, sizeof(hhmm), false);
    widest_updated_text(dated, sizeof(dated), true);
    const char *texts[] = {hhmm, dated, "--:--"};

    for (size_t i = 0u; i < sizeof(texts) / sizeof(texts[0]); ++i) {
        g_updated_text = texts[i];

        air_reading_t r = widest_numbers();
        r.temperature_c.accuracy = ACC_LOW;
        render(&r);
        check_layout(texts[i]);  // beside "Temp. not calibrated", the longest single note
        r.temperature_c.accuracy = ACC_NOT_REPORTED;
        r.humidity_rh.accuracy = ACC_LOW;
        render(&r);
        check_layout(texts[i]);  // beside "Humidity not calibrated"

        r.co2_ppm.accuracy = ACC_LOW;
        r.temperature_c.accuracy = ACC_LOW;
        r.humidity_rh.accuracy = ACC_LOW;
        r.pressure_hpa.accuracy = ACC_LOW;
        r.iaq = measurement_ok(200.0f, RENDER_TS_MS, SOURCE_ID);
        r.iaq.accuracy = ACC_LOW;
        render(&r);
        check_layout(texts[i]);  // beside "5 not calibrated"

        r.co2_ppm = measurement_status(VAL_ERROR, RENDER_TS_MS, SOURCE_ID);
        r.temperature_c = measurement_status(VAL_ERROR, RENDER_TS_MS, SOURCE_ID);
        r.humidity_rh = measurement_status(VAL_ERROR, RENDER_TS_MS, SOURCE_ID);
        r.pressure_hpa = measurement_status(VAL_ERROR, RENDER_TS_MS, SOURCE_ID);
        render(&r);
        check_layout(texts[i]);  // beside the box around "4 sensor errors"

        r.co2_ppm = measurement_status(VAL_WARMUP, RENDER_TS_MS, SOURCE_ID);
        render(&r);
        check_layout(texts[i]);  // beside the box around "3 sensor errors"

        g_updated_text = NULL;
    }
}

// IAQ in its widest bands -- "Moderate", then "Excellent" -- beside the
// question mark of a scale not yet calibrated, and the widest number, "500",
// in "Poor" (docs/adr/0021-measurement-accuracy-and-iaq-levels.md).
static void the_widest_iaq_bands_fit(void)
{
    const float values[] = {200.0f, 50.0f, 500.0f};
    for (size_t i = 0u; i < sizeof(values) / sizeof(values[0]); ++i) {
        air_reading_t r = widest_numbers();
        r.iaq = measurement_ok(values[i], RENDER_TS_MS, SOURCE_ID);
        r.iaq.accuracy = ACC_LOW;
        render(&r);
        check_layout("widest IAQ bands, not calibrated");
    }
}

// The footer's longest word on accuracy: one reading not calibrated, named
// by the longest label, and every grid reading at once.
static void the_longest_calibration_notes_fit(void)
{
    air_reading_t r = widest_numbers();
    r.temperature_c.accuracy = ACC_LOW;
    render(&r);
    check_layout("Temp. not calibrated");

    r.co2_ppm.accuracy = ACC_LOW;
    r.humidity_rh.accuracy = ACC_LOW;
    r.pressure_hpa.accuracy = ACC_LOW;
    r.iaq = measurement_ok(200.0f, RENDER_TS_MS, SOURCE_ID);
    r.iaq.accuracy = ACC_LOW;
    render(&r);
    check_layout("5 not calibrated");
}

// The headline's unit and mark never share a pixel with any reading, and
// never move. When "ppm" followed the reading, a four-digit reading was drawn
// over the place a three-digit one had left the unit, and the unit was drawn
// somewhere new: a partial refresh drove the letters' pixels each time, and
// the panel kept them (screen_home.c, draw_headline()).
//
// The reading's own ink is what differs between the screen drawn with it and
// without it; everything else in the headline -- label, unit, mark -- is what
// the screen draws without it.

#define HEAD_TOP (SCREEN_HOME_BAR_RULE_Y + SCREEN_HOME_RULE_PX)
#define HEAD_LEFT SCREEN_HOME_MARGIN
#define HEAD_RIGHT (PANEL_W - SCREEN_HOME_MARGIN - SCREEN_HOME_BANNER_W)

static uint8_t g_full[PANEL_H][PANEL_W];
static bool g_reading_ink[PANEL_H][PANEL_W];  // any reading's, so far
static uint8_t g_rest[PANEL_H][PANEL_W];      // the first OK reading's rest

static void render_headline_parts(const air_reading_t *r, bool record_rest, bool *rest_same)
{
    view_model_t vm;
    build(&vm, r);
    screen_home_render(&g_fb, &vm);
    for (int32_t y = HEAD_TOP; y < SCREEN_HOME_HEAD_RULE_Y; ++y) {
        for (int32_t x = HEAD_LEFT; x < HEAD_RIGHT; ++x) {
            g_full[y][x] = framebuffer_get_pixel(&g_fb, x, y);
        }
    }
    vm.fields[VM_FIELD_CO2].text[0] = '\0';
    screen_home_render(&g_fb, &vm);
    for (int32_t y = HEAD_TOP; y < SCREEN_HOME_HEAD_RULE_Y; ++y) {
        for (int32_t x = HEAD_LEFT; x < HEAD_RIGHT; ++x) {
            const uint8_t rest = framebuffer_get_pixel(&g_fb, x, y);
            if (g_full[y][x] != rest) {
                g_reading_ink[y][x] = true;
            }
            if (record_rest) {
                g_rest[y][x] = rest;
            } else if (rest_same != NULL && rest != g_rest[y][x]) {
                *rest_same = false;
            }
        }
    }
}

static void the_co2_unit_and_mark_stay_off_every_reading(void)
{
    memset(g_reading_ink, 0, sizeof g_reading_ink);

    // Readings with no mark: the rest of the headline is the same for all.
    const float readings[] = {938.0f, 400.0f, 1002.0f, 9999.0f, 40000.0f};
    for (size_t i = 0u; i < sizeof(readings) / sizeof(readings[0]); ++i) {
        air_reading_t r = widest_numbers();
        r.co2_ppm = measurement_ok(readings[i], RENDER_TS_MS, SOURCE_ID);
        bool same = true;
        render_headline_parts(&r, i == 0u, &same);
        HAC_CHECK(same);  // "CO2 ppm" where it was
    }
    // And the placeholders, whose marks join the rest.
    const value_status_t statuses[] = {VAL_ERROR, VAL_WARMUP, VAL_UNAVAILABLE};
    for (size_t i = 0u; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
        air_reading_t r = widest_numbers();
        r.co2_ppm = measurement_status(statuses[i], RENDER_TS_MS, SOURCE_ID);
        render_headline_parts(&r, false, NULL);
    }

    // No ink but the reading's lies where any reading can be: checked for the
    // first reading's rest and for each placeholder's, rendered again.
    bool apart = true;
    for (size_t k = 0u; k <= sizeof(statuses) / sizeof(statuses[0]); ++k) {
        air_reading_t r = widest_numbers();
        r.co2_ppm = (k == 0u) ? measurement_ok(readings[0], RENDER_TS_MS, SOURCE_ID)
                              : measurement_status(statuses[k - 1u], RENDER_TS_MS, SOURCE_ID);
        view_model_t vm;
        build(&vm, &r);
        vm.fields[VM_FIELD_CO2].text[0] = '\0';
        screen_home_render(&g_fb, &vm);
        for (int32_t y = HEAD_TOP; y < SCREEN_HOME_HEAD_RULE_Y; ++y) {
            for (int32_t x = HEAD_LEFT; x < HEAD_RIGHT; ++x) {
                if (g_reading_ink[y][x] && inked(x, y)) {
                    apart = false;
                }
            }
        }
    }
    HAC_CHECK(apart);
}

int main(void)
{
    const size_t size = framebuffer_required_size(PANEL_W, PANEL_H, PIXFMT_GRAY4);
    g_pixels = malloc(size);
    if (g_pixels == NULL) {
        return 1;
    }
    framebuffer_init(&g_fb, g_pixels, size, PANEL_W, PANEL_H, PIXFMT_GRAY4);
    g_zone = time_zone_eu(120);  // Vilnius

    HAC_RUN(the_widest_numbers_fit);
    HAC_RUN(every_field_warming_up_fits);
    HAC_RUN(every_field_failed_fits);
    HAC_RUN(nothing_provided_fits);
    HAC_RUN(the_widest_numbers_gone_stale_fit);
    HAC_RUN(the_widest_iaq_bands_fit);
    HAC_RUN(the_longest_calibration_notes_fit);
    HAC_RUN(the_widest_update_times_fit);
    HAC_RUN(the_co2_unit_and_mark_stay_off_every_reading);

    free(g_pixels);
    return hac_test_summary();
}
