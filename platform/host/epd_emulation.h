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
// or this one well away from 21 C: e-paper timing depends on both. The clear
// and the full refresh are the exception since ADR 0035 changed both: ESTIMATES
// from those measurements, each saying so at its definition, until the board
// times the new ones.
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

// REFRESH_CLEAR -> the vendor's clear (platform/esp32_t5s3/board_config.h,
// BOARD_EPD_CLEAR_*; docs/adr/0035-the-vendors-full-refresh.md): four cycles of
// four whole-panel pushes towards black and four towards white, every row
// driven for 50 us. Content plays no part. ESTIMATE until the board's
// `adp_display: panel cleared took_ms=` replaces it (board_notes.md, open
// item "The vendor's full refresh"), from two measured figures: a push costs
// 7.6 ms of its own -- EPDiy's 66-push clear fitted 502 ms + 3.607 ms per row
// (ADR 0010) -- and a row costs about 10 us beyond its drive -- DU's 100 us
// rows took 109.6 us (ADR 0008). So 32 x (7.6 ms + 540 x 60 us) = 1,280 ms.
// EPDiy's own clear, which this replaced, measured 2,456 ms (ADR 0008).
#define EPD_SIM_CLEAR_REFRESH_MS 1280u

// REFRESH_FULL -> the clear, then MODE_EPDIY_WHITE_TO_GL16 over the whole
// panel. It writes only rows that are not white once the clear has made
// EPDiy's model white, as GL16 does. Its 15 passes drive a row for 3-30 us,
// GL16's 30 for 1-30 us: both under the ~53 us a row the data bus takes
// whatever the drive (GL16's 1.601 ms per row over 30 passes), so a pass of
// one costs what a pass of the other does -- half GL16's base and half its
// per-written-row rate. ESTIMATE, derived from GL16's measurement, until
// EPDiy's own timing line for the board's full refresh replaces it. So a full
// refresh is EPD_SIM_CLEAR_REFRESH_MS, the whole panel's difference pass and
// this over the frame's written rows: about 1,936 ms for a solid fill, against
// 3,661 measured with EPDiy's clear and GC16 (ADR 0008).
#define EPD_SIM_FULL_DRAW_BASE_US (EPD_SIM_PARTIAL_BASE_US / 2u)
#define EPD_SIM_FULL_DRAW_PER_ROW_US (EPD_SIM_PARTIAL_PER_ROW_US / 2u)

// What the clear looks like, and so what the window shows during it: each
// cycle pushes the panel towards black 4 times, then towards white 4 times --
// eight flashes in all, as the owner counted on the vendor's firmware. From
// the vendor's source; the window shares EPD_SIM_CLEAR_REFRESH_MS out over the
// pushes, about 40 ms each. Then nothing changes while EPDiy works out the
// difference, and the picture darkens in from white: this draw never takes a
// pixel through black first, as GC16 did.
#define EPD_SIM_CLEAR_CYCLES 4u
#define EPD_SIM_CLEAR_DARK_PUSHES 4u
#define EPD_SIM_CLEAR_WHITE_PUSHES 4u

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
// digits every step, and around 25-30 in the realistic sawtooth; each full
// refresh removed the outlines. Phase 2c set the budget one below the
// earliest of those, 10. Since 2026-10-16 it is 20, the owner's choice
// (ADR 0019): half of what is invisible from 50 cm, trading close-up outlines
// from the 11th, which the window shows, for fewer full refreshes.
#define EPD_SIM_MAX_PARTIAL_REFRESHES_BEFORE_FULL 20u

// OBSERVED: the most partial refreshes this panel has been driven through
// without a full one -- the ghosting probes' 40 (ADR 0008), with nothing
// visible from 50 cm. Past the budget update_policy keeps refreshing
// partially until its minimum full-refresh interval allows a full one (ADR
// 0019) -- never at the firmware's pace of one redraw per 5 s cycle, up to
// three past a budget of 20 at twice that; the emulated panel turns a
// partial refresh into a full one only past this, where nothing about the
// glass is known.
#define EPD_SIM_MOST_PARTIALS_TESTED 40u

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
// Kept out of clang-format: versions disagree on how to lay out a braced list
// in a macro, and CI's must pass as well as the local one.
// clang-format off
#define EPD_SIM_LEVEL_LIGHTNESS_PERMILLE \
    {0, 0, 47, 156, 273, 344, 430, 500, 547, 594, 656, 725, 794, 862, 931, 1000}
// clang-format on

// --- presentation ----------------------------------------------------------

// The flushed rectangle is outlined for at least this long even when the
// flush itself is shorter, so a fast partial update is still visible as one.
// A property of the window, not of the panel: no measurement changes it.
#define EPD_SIM_HIGHLIGHT_MIN_MS 90u

#endif  // HAC_PLATFORM_HOST_EPD_EMULATION_H
