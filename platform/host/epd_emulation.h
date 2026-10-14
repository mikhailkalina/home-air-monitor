// SPDX-License-Identifier: Apache-2.0
//
// Every number the simulator uses to imitate the ED047TC1, in one place.
//
// MEASURED, phase 2c (2026-10-13), on the one unit this project owns --
// silkscreen Screen-4.7-S3 v2.4, ambient 21 C -- by the firmware's calibration
// mode (apps/firmware_esp32/main/epd_calibration.c), which times
// port_display->flush() with esp_timer: the same call the core waits on and
// this file emulates, power bracket and epd_clear() included.
// docs/adr/0008-panel-timing.md holds the raw table, the fits and the margins
// tests/unit/test_epd_model.c holds the model to. Re-measure on a second unit,
// or this one well away from 21 C: e-paper timing depends on both.
//
// They are gathered here, rather than spread through the adapters, so that
// replacing a measurement is an edit to this file and nothing else.
//
// What the emulation is FOR is not visual fidelity. The vendor states that
// prolonged partial refreshing leaves residual images and causes irreversible
// damage to this panel. A refresh strategy bad enough to do that must become
// visible on a PC, in the log and in the window, long before it reaches
// glass.

#ifndef HAC_PLATFORM_HOST_EPD_EMULATION_H
#define HAC_PLATFORM_HOST_EPD_EMULATION_H

// --- refresh latency -------------------------------------------------------
//
// A refresh costs by ROWS, never by area -- 960x228 and 240x228 measured
// 646.8 and 646.6 ms -- and by two kinds of row:
//
//   * EPDiy first computes the difference between the new frame and its model
//     of the glass over every row of the flushed rectangle. Its own log puts
//     that at 106 ms for 540 rows, 45 for 228, 12 for 60: EPD_SIM_DIFF_PER_ROW_US
//     per rectangle row, whether anything in the row changed or not.
//   * Then each waveform pass visits every panel row, writing whole rows that
//     hold a changed pixel -- plus the first two of every run of unchanged
//     rows, while its pipeline drains -- and clocking the rest past cheaply
//     (epd_model.c, written_rows()). That costs a base per refresh plus a
//     per-written-row rate.
//
// Base and per-written-row rate are fitted, after taking the difference pass
// out, through the 60- and 540-row means (62 and 541 written rows). Against
// the measurements: the 228-row cases within 0.13 %, the product's own
// headline rectangles within 0.3 %, and screen_home's full refresh -- 334
// written rows in many separate bands of text -- within 0.2 %. Counting the
// rectangle's height instead would put the product's typical partial refresh
// (an 824 x 228 rectangle with about 90 changed rows) at 646 ms; the panel
// takes about 400.
//
// Rows here means the rows the drive actually touches: EPDiy drives only rows
// in which a pixel changed, and skips the rest of the flushed rectangle. The
// product's typical partial refresh -- the CO2 headline plus the temperature
// and humidity values, an 824 x 228 rectangle -- touches about 90 of its 228
// rows and takes about 400 ms on the panel; counting the whole rectangle would
// say 646. epd_model.c counts the rows from the frame.
//
// Kept in microseconds so the per-row slope keeps its precision; the model
// rounds the total to whole milliseconds.

// EPDiy's difference pass, per row of the flushed rectangle.
#define EPD_SIM_DIFF_PER_ROW_US 196u

// REFRESH_PARTIAL -> MODE_GL16, 30 waveform passes; per written row.
#define EPD_SIM_PARTIAL_BASE_US 233977u
#define EPD_SIM_PARTIAL_PER_ROW_US 1601u

// REFRESH_FAST_MONO -> MODE_DU, 5 waveform passes; per written row.
#define EPD_SIM_FAST_MONO_BASE_US 40711u
#define EPD_SIM_FAST_MONO_PER_ROW_US 548u

// REFRESH_CLEAR -> epd_clear(): three cycles of 22 whole-panel pushes.
// Content plays no part; five samples spanned 32 us.
#define EPD_SIM_CLEAR_REFRESH_MS 2456u

// REFRESH_FULL -> epd_clear() and then MODE_GC16 over the whole panel. GC16
// has the same 30 passes as GL16 and costs the same (1,204.7 ms after the
// clear for 540 rows, against GL16's 1,206.1), and it writes only rows that
// are not white once the clear has made EPDiy's model white. So a full
// refresh is EPD_SIM_CLEAR_REFRESH_MS plus the partial formula over the whole
// panel's difference pass and the frame's written rows, which the model counts
// from the frame itself: 3,661 ms for a solid fill, 3,325 ms for screen_home.

// What the clear looks like, and so what the window shows during it: EPDiy's
// epd_clear() is epd_clear_area_cycles(area, 3, ...) (render.c), and each of
// the three cycles pushes the area towards black 10 times, then towards white
// 10 times, then 2 neutral pushes. Taken from the source, not measured; the
// measured part is the total, EPD_SIM_CLEAR_REFRESH_MS, which the window
// shares out over the pushes -- about 37 ms each, so roughly 0.37 s black and
// 0.45 s white, three times. The remainder of a full refresh is the image
// drawing in.
#define EPD_SIM_CLEAR_CYCLES 3u
#define EPD_SIM_CLEAR_DARK_PUSHES 10u
#define EPD_SIM_CLEAR_WHITE_PUSHES 12u  // 10 towards white + 2 neutral

// What the glass shows while GC16 draws the image after the clear, and so what
// the window shows then. GC16 takes each pixel it drives towards black for the
// first half of its passes and to its own level in the second -- 15 and 15 for
// white to white (docs/hardware/board_notes.md, "Vertical stripes in white
// after a full refresh"). After the clear it drives a pixel only where its row
// holds a non-white pixel of the new frame (the rows it writes) and so does its
// column (EPDiy's one, global column mask). Seen on the panel 2026-10-14:
// after the clear's flashing the screen goes black, then the picture appears.
// From the source and that observation; only the total is timed, and the
// window gives the dark passes their share of the draw.
#define EPD_SIM_GC16_PASSES 30u
#define EPD_SIM_GC16_DARK_PASSES 15u

// --- panel constraints reported up to the core -----------------------------
//
// These two reach core/app/update_policy.c through port_display, and are the
// only reason it can protect the panel without knowing which panel it is.
// platform/esp32_t5s3/board_config.h reports the same two values on the
// device; tests/unit/test_panel_limits_agree.c fails the build if they drift
// apart, because then the device and the simulator would run different
// policies.
//
// OBSERVED, not timed: the partial-refresh budget comes from two ghosting
// probes watched on the panel (ADR 0008). Viewed from 50 cm nothing was
// visible through 40 partial refreshes in either pattern. Looked at closely,
// light outlines of earlier digits appeared inside the current ones after
// 11-25 partial refreshes when the headline flipped between three and four
// digits every step, and around 25-30 in the realistic sawtooth. The budget
// is one below the earliest of those, so a policy that respects it leaves
// nothing visible even close up; each full refresh removed the outlines.
#define EPD_SIM_MAX_PARTIAL_REFRESHES_BEFORE_FULL 10u

// A GUARD, NOT A MEASUREMENT, and not a measurable one: no stopwatch says how
// often a panel "tolerates" a full refresh, and the vendor gives no figure.
// It rate-limits the 3.7 s flashing refresh rather than protecting anything
// known. No phase is scheduled to replace it; a datasheet figure would.
#define EPD_SIM_MIN_FULL_REFRESH_INTERVAL_MS 60000u

// --- ghosting --------------------------------------------------------------
//
// What the probes actually showed, and so what the window imitates: not a
// dark smudge where a pixel used to be, but light outlines of an earlier
// digit INSIDE the current one. EPDiy's partial refresh drives only the
// pixels that changed; dark pixels that stay dark through successive partial
// refreshes are never redriven and end up lighter than their freshly driven
// neighbours, so the earlier glyph's shape shows through. A full refresh
// redrives everything and the outlines vanish.
//
// The window counts, per pixel, the partial refreshes since that pixel was
// last driven (only refreshes whose rectangle covered it). Once a dark pixel
// has gone unredriven for EPD_SIM_GHOST_VISIBLE_AFTER of them, it is shown
// EPD_SIM_GHOST_LIGHTEN lighter, in the window's 8-bit grey -- about one
// nominal grey step, faint, because what was seen stayed faint -- "noticeable
// only when looking closely" -- all the way to 40.
//
// VISIBLE_AFTER is the observed onset's lower bound (11). Whether the
// outlines deepen past one step with many more partial refreshes was not
// observed, so they do not here.
#define EPD_SIM_GHOST_VISIBLE_AFTER 11u
#define EPD_SIM_GHOST_LIGHTEN 17u
// "Dark" for the purpose above: GRAY4 levels below this. The outlines were
// seen in black digits; mid greys were not examined.
#define EPD_SIM_GHOST_DARK_BELOW 8u

// --- grey levels -----------------------------------------------------------
//
// How light each GRAY4 level is on the glass, in permille of the way from the
// panel's black (level 0) to its white (level 15): relative reflectance,
// linear in light, so a fine mix of two levels is as light as the average of
// theirs. The window and the headless frames show each level as this
// (epd_level_display_grey() in epd_model.c), and
// apps/tools/image_gen/image_gen.py prepare reads this very array to dither
// photographs towards the panel's real greys -- one table, so a picture looks
// on the PC as it will on the glass (docs/adr/0013-grey-level-lightness.md).
//
// MEASURED by eye on this unit, 2026-10-14, levels 1-10: each level matched
// against fine mixes of levels 0 and 15 on the level-match chart
// (apps/slideshow/images/06_level_match.png), in room light from viewing
// distance; docs/hardware/board_notes.md, "How light each grey level really
// is", has the readings. Level 1 matched black: it is phase 2c's "levels 0
// and 1 look identical", and the window now shows it so through this table.
// Levels 4-10 lie within 3 points of even steps in reflectance; the
// uncalibrated L x 17 this replaces had level 7 at 18 % where it measures 50.
//
// ESTIMATED, levels 11-14: the chart could not match them -- sparse black dots
// on white render darker than their share on e-paper -- so they are spaced
// evenly between level 10 and white, as levels 4-10 are. A measurement free of
// that effect replaces them: a RAW photograph of the solid ramp, or a chart
// mixing levels 10 and 15. No branch has it planned yet.
#define EPD_SIM_LEVEL_LIGHTNESS_PERMILLE \
    {0, 0, 47, 156, 273, 344, 430, 500, 547, 594, 656, 725, 794, 862, 931, 1000}

// --- presentation ----------------------------------------------------------

// The flushed rectangle is outlined for at least this long even when the
// flush itself is shorter, so a fast partial update is still visible as one.
// A property of the window, not of the panel: no measurement changes it.
#define EPD_SIM_HIGHLIGHT_MIN_MS 90u

#endif  // HAC_PLATFORM_HOST_EPD_EMULATION_H
