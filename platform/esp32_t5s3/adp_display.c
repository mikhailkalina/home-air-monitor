// SPDX-License-Identifier: Apache-2.0

#include "adp_display.h"

#include <math.h>
#include <string.h>

// epdiy.h pulls in ESP-IDF's xtensa/core-macros.h, whose
// XTHAL_COMPARE_AND_SET() is declared `static inline unsigned` and ends in
// `return result;` with `result` an `int` (components/xtensa/include/xtensa/
// core-macros.h:455 in ESP-IDF 5.5). GCC compiles static inline bodies in
// whichever translation unit includes them, so that sign conversion is this
// file's -Werror=sign-conversion failure even though neither this file nor
// EPDiy wrote it.
//
// Suppressed around the include only, and only for that one diagnostic.
// cmake/warnings_idf.cmake's own comment offers the alternative -- dropping
// -Wconversion for the whole component -- but hac_platform also builds
// adp_clock.c, adp_log.c and adp_display_null.c, and none of those should
// lose the flag to accommodate a header they do not include. Revisit if a
// later ESP-IDF fixes the header: the pragma is then dead and should go.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include "epdiy.h"
#pragma GCC diagnostic pop

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"

#include "board_config.h"
#include "epd_white_path.h"

// The panel this adapter drives. Named once and used by both epd_init() and
// epd_hl_init(), so the two can never disagree about which panel -- and
// therefore which waveform -- is in play.
static const EpdDisplay_t *const s_panel = &ED047TC1;

// EPDiy's highlevel state: the front buffer the core draws into, the back
// buffer holding EPDiy's model of what the glass shows, and the difference
// buffer between them. Module scope rather than a field of adp_display_t
// because EPDiy asserts on a second epd_hl_init() anyway -- one panel, one
// state.
//
// Going through the highlevel API rather than epd_draw_base() directly is not
// a convenience: it is the only path that has the information the panel needs.
// epd_draw_base() with MODE_PACKING_2PPB selects no lookup function at all
// unless the mode also carries PREVIOUSLY_WHITE or PREVIOUSLY_BLACK
// (output_common/lut.c's find_lut_functions()), because a whole-buffer draw
// with no diff has to be told which state every pixel is coming from. The
// highlevel layer knows, because it keeps the back buffer; it computes the
// difference and asks for MODE_PACKING_1PPB_DIFFERENCE, for which a lookup
// does exist. An earlier revision of this adapter called epd_draw_base()
// directly and failed on every flush with EPD_DRAW_LOOKUP_NOT_IMPLEMENTED.
//
// This does not duplicate core/ui/framebuffer.c's own diff. The two answer
// different questions: framebuffer_diff_dirty_rect() decides *whether and
// which region* to refresh, which is policy (core/app/update_policy.c acts on
// it); EPDiy's difference decides *how to drive each pixel inside that
// region*, which needs the previous pixel value and is waveform business.
static EpdiyHighlevelState s_hl;

// The partial refresh's mode, "through white" (epd_white_path.h), added at
// boot to EPDiy's waveform for this panel under a type number that waveform
// does not use: get_waveform_index() (render.c) matches `mode & 0x3F` against
// each mode's type and nothing else in EPDiy looks at it. EPDiy's own types
// for this panel are 1, 2, 5, 16 and 17.
#define MODE_HAC_WHITE_PATH ((enum EpdDrawMode)0x12)

// The waveform handed to epd_hl_init(): EPDiy's modes for this panel and the
// one above. Room for EPDiy's five and ours with some to spare.
#define WAVEFORM_MAX_MODES 8u

static epd_white_path_t s_white_path;
static EpdWaveformPhases s_white_path_phases;
static const EpdWaveformPhases *s_white_path_ranges[1];
static EpdWaveformMode s_white_path_mode;
static const EpdWaveformMode *s_waveform_modes[WAVEFORM_MAX_MODES];
static EpdWaveform s_waveform;

// What a partial and a full refresh draw with: through white and from white
// when the mode above could be built, GL16 for both when it could not -- the
// two must take greys the same way (ADR 0035, second amendment).
static enum EpdDrawMode s_partial_mode = MODE_GL16;
static enum EpdDrawMode s_full_draw_mode = MODE_GL16;

// One of EPDiy's modes for this panel by its type, in the pure module's
// terms; false if the waveform has no such mode or more than the one
// temperature range this one has.
static bool mode_of(const EpdWaveform *w, uint8_t type, epd_white_path_mode_t *out)
{
    if (w == NULL || w->num_temp_ranges != 1) {
        return false;
    }
    for (int i = 0; i < w->num_modes; ++i) {
        const EpdWaveformMode *m = w->mode_data[i];
        if (m->type == type && m->temp_ranges == 1 && m->range_data[0] != NULL) {
            const EpdWaveformPhases *ph = m->range_data[0];
            out->phases = (size_t)ph->phases;
            out->luts = ph->luts;
            out->times = ph->phase_times;
            return true;
        }
    }
    return false;
}

// EPDiy's waveform for this panel with the mode "through white" added, or
// EPDiy's own and GL16 for both kinds of refresh if it cannot be built.
static const EpdWaveform *waveform_for_this_panel(const port_log_t *log)
{
    const EpdWaveform *base = s_panel->default_waveform;
    epd_white_path_mode_t gl16;
    epd_white_path_mode_t white_to;
    const bool found = mode_of(base, (uint8_t)MODE_GL16, &gl16) &&
                       mode_of(base, (uint8_t)MODE_EPDIY_WHITE_TO_GL16, &white_to);
    if (!found || base->num_modes >= WAVEFORM_MAX_MODES ||
        epd_white_path_build(&gl16, &white_to, &s_white_path) != 0) {
        port_log_write(log, LOG_LEVEL_WARN, "adp_display",
                       "no mode through white for this waveform: GL16 for every refresh's "
                       "draw",
                       NULL, 0u);
        return base;
    }

    s_white_path_phases.phases = (int)s_white_path.phases;
    s_white_path_phases.luts = s_white_path.luts;
    s_white_path_phases.phase_times = s_white_path.times;
    s_white_path_ranges[0] = &s_white_path_phases;
    s_white_path_mode.type = (uint8_t)MODE_HAC_WHITE_PATH;
    s_white_path_mode.temp_ranges = 1;
    s_white_path_mode.range_data = s_white_path_ranges;
    for (int i = 0; i < base->num_modes; ++i) {
        s_waveform_modes[i] = base->mode_data[i];
    }
    s_waveform_modes[base->num_modes] = &s_white_path_mode;
    s_waveform.num_modes = (uint8_t)(base->num_modes + 1);
    s_waveform.num_temp_ranges = base->num_temp_ranges;
    s_waveform.mode_data = s_waveform_modes;
    s_waveform.temp_intervals = base->temp_intervals;

    s_partial_mode = MODE_HAC_WHITE_PATH;
    s_full_draw_mode = MODE_EPDIY_WHITE_TO_GL16;
    const log_field_t fields[] = {
        log_u64("phases", s_white_path.phases),
        log_u64("lighten_phases", s_white_path.phases - white_to.phases),
        log_u64("darken_phases", white_to.phases),
    };
    port_log_write(log, LOG_LEVEL_INFO, "adp_display",
                   "partial refreshes through white, full ones darkened from white", fields,
                   sizeof(fields) / sizeof(fields[0]));
    return &s_waveform;
}

static adp_display_t *self_of(port_display_t *port)
{
    return (adp_display_t *)port->impl;
}

static framebuffer_t *adp_get_framebuffer(port_display_t *port)
{
    adp_display_t *d = self_of(port);
    return (d->pixels != NULL) ? &d->fb : NULL;
}

static const char *refresh_mode_name(refresh_mode_t mode)
{
    switch (mode) {
        case REFRESH_FULL:
            return "full";
        case REFRESH_PARTIAL:
            return "partial";
        case REFRESH_FAST_MONO:
            return "fast_mono";
        case REFRESH_CLEAR:
            return "clear";
        default:
            return "?";
    }
}

// EpdDrawError is a bitmask of causes, not an ordinal, so the specific ones
// worth distinguishing are tested before the catch-all. Only the first
// matching cause reaches the core -- hal_status_t carries one code -- which is
// why the raw mask is logged alongside it.
static hal_status_t hal_status_of(enum EpdDrawError err)
{
    if (err == EPD_DRAW_SUCCESS) {
        return HAL_OK;
    }
    if ((err & EPD_DRAW_FAILED_ALLOC) != 0) {
        return HAL_ERR_NO_MEM;
    }
    if ((err & (EPD_DRAW_MODE_NOT_FOUND | EPD_DRAW_INVALID_PACKING_MODE |
                EPD_DRAW_LOOKUP_NOT_IMPLEMENTED | EPD_DRAW_NO_PHASES_AVAILABLE)) != 0) {
        return HAL_ERR_UNSUPPORTED;
    }
    if ((err & EPD_DRAW_INVALID_CROP) != 0) {
        return HAL_ERR_INVALID_ARG;
    }
    return HAL_ERR_IO;
}

// Tells EPDiy's model that the whole glass is white. Only ever called
// immediately after clear_panel() has made that true; anywhere else it would
// make the model lie, and EPDiy would then skip driving pixels it believed
// were already white.
//
// The back buffer is a public field of EpdiyHighlevelState
// (epd_highlevel.h), and there is no API that resets it: epd_hl_set_all_white()
// touches the *front* buffer, which is the one core/ draws into.
static void model_glass_as_white(const adp_display_t *d)
{
    memset(s_hl.back_fb, 0xFF, d->fb.pixels_size);
}

// epd_push_pixels()'s colour argument (epdiy.h): which way a push drives.
#define EPD_PUSH_DARKEN 0
#define EPD_PUSH_LIGHTEN 1

// The clear a full refresh starts with: the vendor's, not EPDiy's
// epd_clear() (board_config.h, BOARD_EPD_CLEAR_*) -- BOARD_EPD_CLEAR_CYCLES
// rounds of the whole panel pushed towards black, then towards white, each
// push driving every row for BOARD_EPD_CLEAR_ROW_US. EPDiy's own clear is
// epd_clear_area_cycles() with ten pushes a phase fixed inside it, so the
// vendor's four need this loop over its public epd_push_pixels(). The glass
// ends white. The caller owns the power bracket. Logs how long it took: the
// figure that replaces the estimate in platform/host/epd_emulation.h.
static void clear_panel(const adp_display_t *d)
{
    const int64_t start_us = esp_timer_get_time();
    const EpdRect whole = epd_full_screen();
    for (int c = 0; c < BOARD_EPD_CLEAR_CYCLES; ++c) {
        for (int i = 0; i < BOARD_EPD_CLEAR_PUSHES_PER_PHASE; ++i) {
            epd_push_pixels(whole, BOARD_EPD_CLEAR_ROW_US, EPD_PUSH_DARKEN);
        }
        for (int i = 0; i < BOARD_EPD_CLEAR_PUSHES_PER_PHASE; ++i) {
            epd_push_pixels(whole, BOARD_EPD_CLEAR_ROW_US, EPD_PUSH_LIGHTEN);
        }
    }
    const log_field_t fields[] = {
        log_u64("took_ms", (uint64_t)((esp_timer_get_time() - start_us) / 1000)),
        log_i64("cycles", BOARD_EPD_CLEAR_CYCLES),
        log_i64("pushes_per_phase", BOARD_EPD_CLEAR_PUSHES_PER_PHASE),
        log_i64("row_us", BOARD_EPD_CLEAR_ROW_US),
    };
    port_log_write(d->log, LOG_LEVEL_INFO, "adp_display", "panel cleared", fields,
                   sizeof(fields) / sizeof(fields[0]));
}

// The one call that drives the panel, with its result logged and mapped.
// The caller owns the power bracket; see adp_flush().
static hal_status_t update_and_log(const adp_display_t *d, enum EpdDrawMode draw_mode, EpdRect area)
{
    const enum EpdDrawError err = epd_hl_update_area(&s_hl, draw_mode, d->temperature_c, area);

    const hal_status_t status = hal_status_of(err);
    const log_field_t fields[] = {
        log_i64("epd_draw_error", (int64_t)err),
        log_i64("status", (int64_t)status),
        log_i64("temperature_c", d->temperature_c),
        log_bool("temperature_measured", d->temperature_measured),
    };
    port_log_write(d->log, LOG_LEVEL_INFO, "adp_display", "epd_hl_update_area returned", fields,
                   sizeof(fields) / sizeof(fields[0]));
    return status;
}

// refresh_mode_t -> EPDiy, and why.
//
// The ED047TC1 waveform EPDiy ships (src/waveforms/epdiy_ED047TC1.h) carries
// exactly five modes, identified by the `type` field get_waveform_index()
// matches against: 1 (MODE_DU), 2 (MODE_GC16), 5 (MODE_GL16), 16 and 17
// (MODE_EPDIY_WHITE_TO_GL16 and _BLACK_TO_GL16). Asking for anything else
// fails at runtime with EPD_DRAW_MODE_NOT_FOUND rather than degrading.
//
// One property of the highlevel path shapes all of this: epd_hl_update_area()
// drives only pixels whose value differs between the front and back buffers.
// Everything that should drive *unchanged* pixels too has to arrange that
// explicitly.
//
//   REFRESH_FULL      -> the vendor's clear (clear_panel()), mark the model
//                        white, then MODE_EPDIY_WHITE_TO_GL16 over the whole
//                        panel -- the vendor's own fifteen darkening frames
//                        (docs/adr/0035-the-vendors-full-refresh.md and its
//                        amendments). port_display.h defines a full refresh
//                        as the one that clears accumulated ghosting, and
//                        update_policy forces one for exactly that reason
//                        once the partial budget is spent. A bare redraw
//                        through the highlevel API would not do it:
//                        unchanged pixels would never be driven, so whatever
//                        residue they carry would stay. Clearing first makes
//                        every non-white pixel a difference against a white
//                        model, so the redraw reaches all of them. Always the
//                        whole panel, whatever `area` says -- a partial-area
//                        full refresh is not a thing this port describes.
//                        This is also what synchronises EPDiy's model with
//                        the glass on the first frame: epd_hl_init() assumes
//                        white, which a bistable panel has no obligation to
//                        be at boot. It drives nothing that stays white, so
//                        no stripes in white. Until ADR 0035 this was EPDiy's
//                        epd_clear() and MODE_GC16: 3.3-3.7 s, residue in
//                        greys (ADR 0008), stripes in white.
//   REFRESH_PARTIAL   -> MODE_HAC_WHITE_PATH over `area`: a changed pixel
//                        driven to white and darkened to its level as the
//                        full refresh darkens it, an unchanged one not driven
//                        at all (epd_white_path.h). No flash beyond the
//                        changed pixels, and ghosting accumulates in what is
//                        not redriven -- port_display.h's definition of
//                        partial. EPDiy clips the far edge of `area` to the
//                        panel itself (epd_difference_image_base()), and
//                        rect_t's unsigned fields cannot produce a negative
//                        near edge. Until ADR 0035's second amendment this
//                        was MODE_GL16, which takes greys through black and
//                        drives an unchanged grey L towards black and back
//                        in every row EPDiy writes and every column of its
//                        global mask (epdiy_ED047TC1.h, mode 5, its L -> L
//                        entries): a grey any full refresh had drawn changed
//                        there, and the static greys beside a changed digit
//                        went lighter in patches -- with GC16's full refresh
//                        unseen (its greys and GL16's share a path), with
//                        WHITE_TO_GL16's at once, with the vendor's clear and
//                        GL16 still a little. If the mode cannot be built,
//                        both kinds of refresh draw with GL16 again.
//   REFRESH_CLEAR     -> the vendor's clear and mark the model white. The glass ends
//                        white; the front buffer, which is core/'s rendered
//                        screen, is deliberately left alone. Nothing issues
//                        REFRESH_CLEAR today (update_action_t has no clear
//                        action, and the bring-up test pattern uses
//                        REFRESH_FULL). A caller that starts using it must
//                        follow it with REFRESH_FULL: apps/firmware_esp32/
//                        main/event_loop.c's previous-frame copy still holds
//                        the old image, so a partial would see no difference
//                        and leave the glass white.
//   REFRESH_FAST_MONO -> MODE_DU over `area`. Five passes against GL16's
//                        thirty, which is the whole point of the mode -- and
//                        the price is that DU only knows two destinations.
//                        Its lookup table (epdiy_ED047TC1.h, mode type 1)
//                        drives a pixel towards black when the target is 0x0
//                        and towards white when it is 0xF, and drives nothing
//                        at all for targets 0x1-0xE. Through the highlevel
//                        API that makes grey content worse than wrong: the
//                        glass keeps its old value while EPDiy's model
//                        records the new grey, so a later partial believes
//                        those pixels are already there. A caller asking for
//                        REFRESH_FAST_MONO must therefore have drawn only
//                        0x0 and 0xF inside `area`. Nothing in the product
//                        asks for it (update_action_t has no such action);
//                        apps/firmware_esp32/main/epd_calibration.c does, to
//                        time it, and draws black and white only.
//
// POWER. Every path brackets its panel drive with epd_poweron() and
// epd_poweroff() itself. EPDiy's highlevel layer does NOT: highlevel.c
// contains no call to either, and its own example for this board
// (examples/lilygo-t5-s3/main/main.c) brackets every update explicitly. An
// earlier revision of this file claimed the opposite in a comment and dropped
// the bracket, which would have driven an unpowered panel on every flush --
// returning EPD_DRAW_SUCCESS while showing nothing.
static hal_status_t adp_flush(port_display_t *port, const rect_t *area, refresh_mode_t mode)
{
    adp_display_t *d = self_of(port);

    if (d->pixels == NULL) {
        return HAL_ERR_NOT_READY;
    }

    const rect_t whole_panel = {0u, 0u, d->port.width, d->port.height};
    const rect_t resolved = (area != NULL) ? *area : whole_panel;

    const log_field_t dirty_fields[] = {
        log_u64("dirty_x", resolved.x),           log_u64("dirty_y", resolved.y),
        log_u64("dirty_w", resolved.w),           log_u64("dirty_h", resolved.h),
        log_str("mode", refresh_mode_name(mode)),
    };
    port_log_write(d->log, LOG_LEVEL_INFO, "adp_display", "flush", dirty_fields,
                   sizeof(dirty_fields) / sizeof(dirty_fields[0]));

    hal_status_t status;
    switch (mode) {
        case REFRESH_FULL:
            epd_poweron();
            clear_panel(d);
            model_glass_as_white(d);
            status = update_and_log(d, s_full_draw_mode, epd_full_screen());
            epd_poweroff();
            return status;

        case REFRESH_PARTIAL:
        case REFRESH_FAST_MONO: {
            const EpdRect partial = {
                .x = (int)resolved.x,
                .y = (int)resolved.y,
                .width = (int)resolved.w,
                .height = (int)resolved.h,
            };
            epd_poweron();
            status =
                update_and_log(d, (mode == REFRESH_PARTIAL) ? s_partial_mode : MODE_DU, partial);
            epd_poweroff();
            return status;
        }

        case REFRESH_CLEAR:
            epd_poweron();
            clear_panel(d);
            epd_poweroff();
            model_glass_as_white(d);
            return HAL_OK;

        default:
            return HAL_ERR_UNSUPPORTED;
    }
}

// A pass-through: EPDiy owns the 74HCT4094 rail sequencing and exposes it. No
// caller uses this today (apps/firmware_esp32/main/event_loop.c never calls
// power()).
//
// It does NOT hold the rails up across flushes, and a future caller must not
// assume it does. epd_poweron() and epd_poweroff() are not reference-counted
// -- each is a direct call into the board's own sequence (epdiy.c) -- and
// adp_flush() ends every drive with epd_poweroff(), so a power(true) issued
// before a flush is undone by the end of it. Keeping the panel powered across
// a burst of flushes would need adp_flush() to learn about it; nothing needs
// that yet.
static hal_status_t adp_power(port_display_t *port, bool on)
{
    adp_display_t *d = self_of(port);
    const log_field_t fields[] = {log_bool("on", on)};

    port_log_write(d->log, LOG_LEVEL_INFO, "adp_display", "power", fields, 1u);
    if (on) {
        epd_poweron();
    } else {
        epd_poweroff();
    }
    return HAL_OK;
}

hal_status_t adp_display_init(adp_display_t *d, uint16_t width, uint16_t height,
                              const port_log_t *log)
{
    memset(d, 0, sizeof(*d));
    d->log = log;

    const size_t fb_size = framebuffer_required_size(width, height, PIXFMT_GRAY4);

    // EPD_LUT_64K buys the largest lookup table EPDiy offers, which is what
    // its own example for this board uses; the 8 MB PSRAM budget
    // (docs/hardware/board_notes.md) has room for it many times over.
    epd_init(&epd_board_lilygo_t5_47_s3, s_panel, EPD_LUT_64K);

    // Checked, not assumed: board_config.h and EPDiy's board definition are
    // two independent statements about the same hardware, and a disagreement
    // here means one of them is wrong about the panel rather than that the
    // adapter should adapt.
    if (epd_width() != (int)width || epd_height() != (int)height) {
        const log_field_t fields[] = {
            log_i64("epd_width", epd_width()),
            log_i64("epd_height", epd_height()),
            log_u64("expected_width", width),
            log_u64("expected_height", height),
        };
        port_log_write(log, LOG_LEVEL_ERROR, "adp_display",
                       "EPDiy reports a different panel geometry than board_config.h", fields,
                       sizeof(fields) / sizeof(fields[0]));
        epd_deinit();
        return HAL_ERR_UNSUPPORTED;
    }

    // This adapter allocates no framebuffer of its own. EPDiy's highlevel
    // front buffer IS the buffer core/ draws into: its GRAY4 nibble order and
    // its width/2 stride are this project's own
    // (docs/adr/0003-gray4-nibble-order.md's second amendment), so wrapping it
    // costs nothing and saves both a 253 KB allocation and a copy per flush.
    // epd_hl_init() memsets front and back to white, which is the state
    // adp_display_null.c and platform/host/adp_display_sdl.c also start from.
    s_hl = epd_hl_init(waveform_for_this_panel(log));
    d->pixels = epd_hl_get_framebuffer(&s_hl);

    framebuffer_init(&d->fb, d->pixels, fb_size, width, height, PIXFMT_GRAY4);
    if (d->fb.pixels == NULL) {
        // framebuffer_init() refuses a buffer smaller than it needs, which
        // here would mean EPDiy and framebuffer_required_size() disagree about
        // the panel -- the same class of mismatch the geometry check above
        // catches, and not something to paper over.
        port_log_write(log, LOG_LEVEL_ERROR, "adp_display",
                       "EPDiy's framebuffer does not fit this panel's GRAY4 size", NULL, 0u);
        return HAL_ERR_UNSUPPORTED;
    }

    // Both figures board_notes.md's memory budget asks to be observed at
    // allocation time rather than inferred: which heap the framebuffer came
    // from, and what EPDiy's own buffers (front, back, difference, the LUT and
    // the line queues) left behind afterwards.
    const log_field_t heap_fields[] = {
        log_bool("framebuffer_in_psram", esp_ptr_external_ram(d->pixels)),
        log_u64("framebuffer_bytes", fb_size),
        log_u64("free_psram_after_epd_init", heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        log_u64("free_internal_after_epd_init", heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
    };
    port_log_write(log, LOG_LEVEL_INFO, "adp_display", "memory budget", heap_fields,
                   sizeof(heap_fields) / sizeof(heap_fields[0]));

    d->temperature_c = BOARD_EPD_ASSUMED_TEMPERATURE_C;  // until a reading arrives
    d->temperature_measured = false;

    d->port.width = width;
    d->port.height = height;
    d->port.format = PIXFMT_GRAY4;
    d->port.min_full_refresh_interval_ms = BOARD_EPD_MIN_FULL_REFRESH_INTERVAL_MS;
    d->port.max_partial_refreshes_before_full = BOARD_EPD_MAX_PARTIAL_REFRESHES_BEFORE_FULL;
    d->port.get_framebuffer = adp_get_framebuffer;
    d->port.flush = adp_flush;
    d->port.power = adp_power;
    d->port.impl = d;

    return HAL_OK;
}

port_display_t *adp_display_port(adp_display_t *d)
{
    return &d->port;
}

void adp_display_set_temperature(adp_display_t *d, bool known, float temperature_c)
{
    const bool measured = known && isfinite(temperature_c);
    const int rounded = measured ? (int)lroundf(temperature_c) : BOARD_EPD_ASSUMED_TEMPERATURE_C;
    if (measured == d->temperature_measured && rounded == d->temperature_c) {
        return;
    }
    // INFO when it switches between measured and assumed; a measured value
    // that only moves a degree -- or flickers across a half -- is DEBUG, and
    // every update's own log line carries the value it used anyway.
    const log_field_t fields[] = {
        log_i64("temperature_c", rounded),
        log_bool("measured", measured),
    };
    port_log_write(d->log, (measured != d->temperature_measured) ? LOG_LEVEL_INFO : LOG_LEVEL_DEBUG,
                   "adp_display", "panel temperature", fields, sizeof(fields) / sizeof(fields[0]));
    d->temperature_c = rounded;
    d->temperature_measured = measured;
}

// EPDiy's highlevel state has no teardown of its own -- epd_hl_init() has no
// counterpart, so its front, back and difference buffers stay allocated. Not
// leaked exactly: epd_hl_init() asserts on a second call, so there is no way
// to allocate them twice, and this firmware never tears the display down
// except on its way to a reset. Recorded here so the asymmetry with
// epd_deinit() reads as EPDiy's shape rather than an omission.
void adp_display_deinit(adp_display_t *d)
{
    epd_deinit();
    d->pixels = NULL;
}
