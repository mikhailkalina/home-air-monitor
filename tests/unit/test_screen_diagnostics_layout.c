// SPDX-License-Identifier: Apache-2.0
//
// The diagnostics screen lays its text out in fixed columns
// (core/ui/screen_diagnostics.h). This holds it to them, as
// test_screen_home_layout.c does the home screen: it renders the widest
// thing each column can say -- "restarting", "failures 999+", "restarts
// 999+", "Temperature", "warm", "unreliable", a source's name as long as a
// name can be, the network's -- "joining", a network's name of 32 octets,
// "-127 dBm", "255.255.255.255", "offline", "sent 999k+", "refused 999+",
// "last 15.10 23:58" (docs/adr/0031-the-network-on-the-screen.md) --
// "Clock: set (--wall-time)" in the title's middle, the devices' lines --
// "starting", "drift -99.9 ppm" -- "Battery 100 % 6600 mV" and "Recording:
// no medium" (docs/adr/0041-touch-rtc-and-battery-on-the-diagnostics-screen.md)
// -- with every line of the sources' section filled, and checks by pixels that
// nothing reaches the margins or a rule, that each column keeps clear of the
// next, that every line stays a line of its own, and that the three parts of
// the title and the two halves of the last line never meet.

#include <stdint.h>
#include <string.h>

#include "hac_test.h"
#include "reporting/reporter.h"
#include "ui/diagnostics_view.h"
#include "ui/fonts/font_dejavu_sans.h"
#include "ui/framebuffer.h"
#include "ui/gfx.h"
#include "ui/screen_diagnostics.h"
#include "ui/view_model.h"

#define PANEL_W 960
#define PANEL_H 540

#define RULE_CLEARANCE 3
#define LINE_CLEARANCE 4
#define HALVES_CLEARANCE 24

static uint8_t g_pixels[PANEL_W / 2 * PANEL_H];
static framebuffer_t g_fb;
static diag_view_t g_view;  // the view rendered last

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

// Every line of the sources' section, every column at its widest: as many
// sources as leave room for both devices, then the devices.
static diag_view_t widest(const char *clock, const char *recorder)
{
    diag_view_t v;
    memset(&v, 0, sizeof(v));
    v.slot_count = SCREEN_DIAG_SOURCE_LINES - DIAG_DEVICE_COUNT;
    for (size_t i = 0u; i < DIAG_MAX_SLOTS; ++i) {
        set(v.slots[i].name, sizeof(v.slots[i].name), "WWWWWWWWWWWWWWWWWWWWWWW");
        v.slots[i].state = "restarting";
        v.slots[i].trouble = true;
        set(v.slots[i].failures, sizeof(v.slots[i].failures), "999+");
        set(v.slots[i].restarts, sizeof(v.slots[i].restarts), "999+");
    }
    // The widest of the devices' words, and of their details: digits share
    // one width in this font, and "-" is wider than any digit's absence.
    static const char *const names[DIAG_DEVICE_COUNT] = {"touch", "rtc"};
    static const char *const details[DIAG_DEVICE_COUNT] = {"at 0xDD", "drift -99.9 ppm"};
    v.device_count = DIAG_DEVICE_COUNT;
    for (size_t i = 0u; i < DIAG_DEVICE_COUNT; ++i) {
        v.devices[i].name = names[i];
        v.devices[i].state = "starting";
        v.devices[i].trouble = true;
        set(v.devices[i].failures, sizeof(v.devices[i].failures), "999+");
        set(v.devices[i].detail, sizeof(v.devices[i].detail), details[i]);
    }
    set(v.battery, sizeof(v.battery), "Battery 100 %");
    set(v.battery_mv, sizeof(v.battery_mv), "6600 mV");  // the ADC's whole range, divided by 2
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

    // The network at its widest. Digits share one width in this font, so one
    // address and one dated time stand for all.
    v.network_present = true;
    v.wifi_state = "joining";
    v.wifi_trouble = true;
    memset(v.ssid, 'W', PORT_NET_SSID_MAX);
    v.ssid[PORT_NET_SSID_MAX] = '\0';
    set(v.rssi, sizeof(v.rssi), "-127 dBm");
    set(v.address, sizeof(v.address), "255.255.255.255");
    v.broker_state = "offline";
    v.broker_trouble = true;
    set(v.sent, sizeof(v.sent), "sent 999k+");
    set(v.refused, sizeof(v.refused), "refused 999+");
    v.refused_trouble = true;
    set(v.last, sizeof(v.last), "last 15.10 23:58");
    return v;
}

// The rows a line of 32px text at baseline `y` inks.
#define LINE_TOP(y) ((y) - 24)
#define LINE_BOTTOM(y) ((y) + 7)

// How far left of its pen position a glyph of the 32px font may ink: a
// column's own text starting with "j" or "J" reaches back into the gap before
// it by that much, which says nothing about the column before.
static int32_t widest_left_bearing(void)
{
    const font_t *font = &font_dejavu_sans_32;
    int32_t widest = 0;
    for (uint16_t i = 0u; i < font->glyph_count; ++i) {
        const int32_t back = -(int32_t)font->glyphs[i].x_offset;
        widest = (back > widest) ? back : widest;
    }
    return widest;
}

// The column boundaries of one section's lines: the strip before each
// column's left end stays clear -- but for what the column's own first glyph
// may reach back into it.
static void check_columns(const int32_t *xs, size_t n, int32_t y0, int32_t y1, const char *what)
{
    const int32_t bearing = widest_left_bearing();
    for (size_t i = 1u; i < n; ++i) {
        const bool ok = clear(xs[i] - SCREEN_DIAG_COLUMN_GAP, y0, xs[i] - 1 - bearing, y1);
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
                       (int)SCREEN_DIAG_SOURCE_LINES,
                   "every source's and device's line stays a line of its own", what, __LINE__);
    hac_test_check(count_bands(m, fields_top, PANEL_W - m - 1, fields_bottom, LINE_CLEARANCE) ==
                       (int)DIAG_FIELD_COUNT,
                   "every reading's line stays a line of its own", what, __LINE__);

    const int32_t slot_cols[] = {SCREEN_DIAG_SLOT_NAME_X, SCREEN_DIAG_SLOT_STATE_X,
                                 SCREEN_DIAG_SLOT_FAILURES_X, SCREEN_DIAG_SLOT_RESTARTS_X};
    check_columns(slot_cols, 4u, slots_top, slots_bottom, what);
    const int32_t field_cols[] = {SCREEN_DIAG_FIELD_LABEL_X, SCREEN_DIAG_FIELD_STATUS_X,
                                  SCREEN_DIAG_FIELD_ACCURACY_X, SCREEN_DIAG_FIELD_SOURCE_X};
    check_columns(field_cols, 4u, fields_top, fields_bottom, what);

    // The network's two lines and the clock's below the last rule.
    const int32_t bottom_top = SCREEN_DIAG_FIELDS_RULE_Y + rule;
    hac_test_check(count_bands(m, bottom_top, PANEL_W - m - 1, PANEL_H - 1, LINE_CLEARANCE) == 3,
                   "the network's lines and the clock's stay lines of their own", what, __LINE__);
    const int32_t wifi_y = SCREEN_DIAG_NET_BASELINE;
    const int32_t mqtt_y = SCREEN_DIAG_NET_BASELINE + SCREEN_DIAG_PITCH;
    const int32_t right = PANEL_W - m;
    const diag_view_t *v = &g_view;
    const int32_t wifi_cols[] = {SCREEN_DIAG_NET_LABEL_X, SCREEN_DIAG_NET_STATE_X,
                                 SCREEN_DIAG_NET_DETAIL_X, SCREEN_DIAG_NET_COUNT_X,
                                 right - gfx_measure_text_ink(&font_dejavu_sans_32, v->address)};
    check_columns(wifi_cols, 5u, LINE_TOP(wifi_y), LINE_BOTTOM(wifi_y), what);
    const int32_t mqtt_cols[] = {SCREEN_DIAG_NET_LABEL_X, SCREEN_DIAG_NET_STATE_X,
                                 SCREEN_DIAG_NET_DETAIL_X, SCREEN_DIAG_NET_COUNT_X,
                                 right - gfx_measure_text_ink(&font_dejavu_sans_32, v->last)};
    check_columns(mqtt_cols, 5u, LINE_TOP(mqtt_y), LINE_BOTTOM(mqtt_y), what);

    // The title's three parts: the clock, centred, keeps clear of both.
    const font_t *font = &font_dejavu_sans_32;
    const int32_t title_end = m + gfx_measure_text_ink(font, "Diagnostics");
    const int32_t clock_w = gfx_measure_text_ink(font, v->clock_text);
    const int32_t clock_x = (PANEL_W - clock_w) / 2;
    const int32_t hint_x = right - gfx_measure_text_ink(font, "tap to return");
    const int32_t title_top = LINE_TOP(SCREEN_DIAG_TITLE_BASELINE);
    const int32_t title_bottom = SCREEN_DIAG_TITLE_RULE_Y - 1;
    ok = clock_x - title_end >= HALVES_CLEARANCE &&
         hint_x - (clock_x + clock_w) >= HALVES_CLEARANCE &&
         clear(title_end, title_top, clock_x - 1 - widest_left_bearing(), title_bottom) &&
         clear(clock_x + clock_w, title_top, hint_x - 1 - widest_left_bearing(), title_bottom) &&
         !clear(clock_x, title_top, clock_x + clock_w - 1, title_bottom);
    hac_test_check(ok, "the title's three parts stay apart", what, __LINE__);
    hac_test_check(widest_gap(LINE_TOP(SCREEN_DIAG_FOOT_BASELINE), PANEL_H - 1) >= HALVES_CLEARANCE,
                   "the last line's halves stay apart", what, __LINE__);
}

static void the_widest_texts_fit(void)
{
    const char *clocks[] = {"Clock: set (--wall-time)", "Clock: set (recording)", "Clock: not set"};
    const char *recorders[] = {"Recording: no medium", "Recording: waiting", "Recording: off"};
    for (size_t i = 0u; i < 3u; ++i) {
        g_view = widest(clocks[i], recorders[i]);
        screen_diagnostics_render(&g_fb, &g_view);
        check_layout(clocks[i]);
    }
}

// Every state word in each line, the longest text in the others; and the
// lines without telemetry.
static void every_network_word_fits(void)
{
    static const char *const wifi[] = {"up", "joining", "failed", "off", "-"};
    static const char *const broker[] = {"online", "offline", "off"};
    for (size_t i = 0u; i < sizeof(wifi) / sizeof(wifi[0]); ++i) {
        for (size_t j = 0u; j < sizeof(broker) / sizeof(broker[0]); ++j) {
            g_view = widest("Clock: set (--wall-time)", "Recording: no medium");
            g_view.wifi_state = wifi[i];
            g_view.broker_state = broker[j];
            screen_diagnostics_render(&g_fb, &g_view);
            check_layout(wifi[i]);
        }
    }
    g_view = widest("Clock: set (rtc)", "Recording: on");
    g_view.network_present = false;
    g_view.wifi_state = "off";
    g_view.broker_state = "off";
    g_view.ssid[0] = '\0';
    g_view.rssi[0] = '\0';
    g_view.address[0] = '\0';
    g_view.sent[0] = '\0';
    g_view.refused[0] = '\0';
    g_view.last[0] = '\0';
    screen_diagnostics_render(&g_fb, &g_view);
    check_layout("no telemetry");
}

// The network's name is shortened to its column, a gap before the signal's.
static void a_long_network_name_is_shortened_to_its_column(void)
{
    g_view = widest("Clock: set", "Recording: on");
    g_view.rssi[0] = '\0';  // nothing in the next column: only the name could be there
    screen_diagnostics_render(&g_fb, &g_view);
    HAC_CHECK(clear(SCREEN_DIAG_NET_COUNT_X - SCREEN_DIAG_COLUMN_GAP,
                    LINE_TOP(SCREEN_DIAG_NET_BASELINE), PANEL_W - SCREEN_DIAG_MARGIN - 300,
                    LINE_BOTTOM(SCREEN_DIAG_NET_BASELINE)));
    HAC_CHECK(!clear(SCREEN_DIAG_NET_DETAIL_X, LINE_TOP(SCREEN_DIAG_NET_BASELINE),
                     SCREEN_DIAG_NET_DETAIL_X + 40, LINE_BOTTOM(SCREEN_DIAG_NET_BASELINE)));
}

static void a_long_name_is_shortened_to_its_column(void)
{
    diag_view_t v = widest("Clock: set", "Recording: on");
    g_view = v;
    screen_diagnostics_render(&g_fb, &v);
    // The name's column ends a gap before the state's.
    const int32_t top = SCREEN_DIAG_TITLE_RULE_Y + SCREEN_DIAG_RULE_PX;
    HAC_CHECK(clear(SCREEN_DIAG_SLOT_STATE_X - SCREEN_DIAG_COLUMN_GAP, top,
                    SCREEN_DIAG_SLOT_STATE_X - 1, SCREEN_DIAG_SLOTS_RULE_Y - 1));
    // And a source's name runs no further than the margin.
    HAC_CHECK(clear(PANEL_W - SCREEN_DIAG_MARGIN, 0, PANEL_W - 1, PANEL_H - 1));
}

static int source_section_lines(const diag_view_t *v)
{
    screen_diagnostics_render(&g_fb, v);
    const int32_t top = SCREEN_DIAG_TITLE_RULE_Y + SCREEN_DIAG_RULE_PX;
    return count_bands(SCREEN_DIAG_MARGIN, top, PANEL_W - SCREEN_DIAG_MARGIN - 1,
                       SCREEN_DIAG_SLOTS_RULE_Y - 1, LINE_CLEARANCE);
}

// No sources: the line that says so, then the devices.
static void no_sources_says_so(void)
{
    diag_view_t v = widest("Clock: set", "Recording: on");
    v.slot_count = 0u;
    v.device_count = 0u;
    HAC_CHECK_EQ_INT(source_section_lines(&v), 1);
    v.device_count = DIAG_DEVICE_COUNT;
    HAC_CHECK_EQ_INT(source_section_lines(&v), 1 + (int)DIAG_DEVICE_COUNT);
}

// The section holds SCREEN_DIAG_SOURCE_LINES lines, the sources first: with as
// many sources as it has lines, the devices are not drawn -- over the rule or
// the readings least of all.
static void the_sources_come_first_and_the_devices_while_room_remains(void)
{
    diag_view_t v = widest("Clock: set", "Recording: on");
    v.slot_count = DIAG_MAX_SLOTS;
    g_view = v;
    HAC_CHECK_EQ_INT(source_section_lines(&v), (int)SCREEN_DIAG_SOURCE_LINES);
    check_layout("four sources, two devices");
    v.slot_count = SCREEN_DIAG_SOURCE_LINES - 1u;
    g_view = v;
    HAC_CHECK_EQ_INT(source_section_lines(&v), (int)SCREEN_DIAG_SOURCE_LINES);
    check_layout("three sources, two devices");
    v.slot_count = 1u;
    v.device_count = 1u;
    HAC_CHECK_EQ_INT(source_section_lines(&v), 2);
}

// The battery's every form at the last line's left, beside the widest
// recorder.
static void every_battery_text_fits(void)
{
    static const char *const texts[] = {"Battery 100 %", "Battery n/a", "Battery ERR",
                                        "Battery stale", "Battery warm"};
    for (size_t i = 0u; i < sizeof(texts) / sizeof(texts[0]); ++i) {
        g_view = widest("Clock: set (rtc)", "Recording: no medium");
        set(g_view.battery, sizeof(g_view.battery), texts[i]);
        screen_diagnostics_render(&g_fb, &g_view);
        check_layout(texts[i]);
        g_view.battery_mv[0] = '\0';
        screen_diagnostics_render(&g_fb, &g_view);
        check_layout(texts[i]);
    }
}

// The held link is not on this screen (docs/adr/0034): the MQTT line says
// it. Same facts, only the held link differs -- the same picture, and
// diag_view_equal() says so, so no redraw is due for it: one due for
// something the screen does not draw is an empty diff, which the loops skip
// and try again -- every second until the screen times out, as the first
// version of the network's lines did (docs/adr/0031).
static void the_held_link_is_not_on_the_screen(void)
{
    static const link_status_t k_links[] = {LINK_UNKNOWN, LINK_NO_WIFI, LINK_NO_BROKER,
                                            LINK_ONLINE};
    enum { N = sizeof(k_links) / sizeof(k_links[0]) };
    static uint8_t frames[N][PANEL_W / 2 * PANEL_H];
    air_reading_t r;
    air_reading_init(&r);
    reporter_status_t n;
    memset(&n, 0, sizeof(n));
    n.link = LINK_NO_BROKER;
    n.has_net = true;
    n.net = NET_UP;
    n.rssi_dbm = -61;
    n.state = REPORTER_OFFLINE;
    static diag_view_t views[N];
    for (size_t i = 0u; i < N; ++i) {
        r.link = k_links[i];
        const diag_facts_t f = {.reading = &r, .network = &n};
        diag_view_build(&views[i], &f);
        screen_diagnostics_render(&g_fb, &views[i]);
        memcpy(frames[i], g_pixels, sizeof(g_pixels));
    }
    for (size_t i = 1u; i < N; ++i) {
        hac_test_check(memcmp(frames[0], frames[i], sizeof(g_pixels)) == 0 &&
                           diag_view_equal(&views[0], &views[i]),
                       link_status_name(k_links[i]), link_status_name(k_links[0]), __LINE__);
    }
}

int main(void)
{
    framebuffer_init(&g_fb, g_pixels, sizeof(g_pixels), PANEL_W, PANEL_H, PIXFMT_GRAY4);
    HAC_RUN(the_widest_texts_fit);
    HAC_RUN(every_network_word_fits);
    HAC_RUN(a_long_network_name_is_shortened_to_its_column);
    HAC_RUN(the_held_link_is_not_on_the_screen);
    HAC_RUN(a_long_name_is_shortened_to_its_column);
    HAC_RUN(no_sources_says_so);
    HAC_RUN(the_sources_come_first_and_the_devices_while_room_remains);
    HAC_RUN(every_battery_text_fits);
    return hac_test_summary();
}
