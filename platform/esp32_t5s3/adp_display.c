// SPDX-License-Identifier: Apache-2.0

#include "adp_display.h"

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

#include "board_config.h"

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
// immediately after epd_clear() has made that true; anywhere else it would
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

// The one call that drives the panel, with its result logged and mapped.
// The caller owns the power bracket; see adp_flush().
static hal_status_t update_and_log(const adp_display_t *d, enum EpdDrawMode draw_mode, EpdRect area)
{
    const enum EpdDrawError err =
        epd_hl_update_area(&s_hl, draw_mode, BOARD_EPD_ASSUMED_TEMPERATURE_C, area);

    const hal_status_t status = hal_status_of(err);
    const log_field_t fields[] = {
        log_i64("epd_draw_error", (int64_t)err),
        log_i64("status", (int64_t)status),
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
//   REFRESH_FULL      -> epd_clear(), mark the model white, then MODE_GC16
//                        over the whole panel. port_display.h defines a full
//                        refresh as the one that clears accumulated
//                        ghosting, and update_policy forces one for exactly
//                        that reason once the partial budget is spent. A
//                        bare GC16 through the highlevel API would not do
//                        it: unchanged pixels would never be driven, so
//                        whatever residue they carry would stay. Clearing
//                        first makes every non-white pixel a difference
//                        against a white model, so the redraw reaches all of
//                        them. Always the whole panel, whatever `area` says
//                        -- a partial-area full refresh is not a thing this
//                        port describes. This is also what synchronises
//                        EPDiy's model with the glass on the first frame:
//                        epd_hl_init() assumes white, which a bistable panel
//                        has no obligation to be at boot.
//   REFRESH_PARTIAL   -> MODE_GL16 over `area`. Sixteen greys without the
//                        clear pass: no flash, and only the changed pixels
//                        are driven, so ghosting accumulates -- port_display.h's
//                        definition of partial, word for word. EPDiy clips the
//                        far edge of `area` to the panel itself
//                        (epd_difference_image_base()), and rect_t's unsigned
//                        fields cannot produce a negative near edge.
//   REFRESH_CLEAR     -> epd_clear() and mark the model white. The glass ends
//                        white; the front buffer, which is core/'s rendered
//                        screen, is deliberately left alone. Nothing issues
//                        REFRESH_CLEAR today (update_action_t has no clear
//                        action, and the bring-up test pattern uses
//                        REFRESH_FULL). A caller that starts using it must
//                        follow it with REFRESH_FULL: apps/firmware_esp32/
//                        main/event_loop.c's previous-frame copy still holds
//                        the old image, so a partial would see no difference
//                        and leave the glass white.
//   REFRESH_FAST_MONO -> HAL_ERR_UNSUPPORTED. The waveform does offer
//                        MODE_DU, so this is reachable should a 1 bpp fast
//                        path ever be wanted -- but nothing requests it and
//                        this adapter runs GRAY4 throughout, so wiring it now
//                        would add a path with no caller and no way to
//                        exercise it.
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
            epd_clear();
            model_glass_as_white(d);
            status = update_and_log(d, MODE_GC16, epd_full_screen());
            epd_poweroff();
            return status;

        case REFRESH_PARTIAL: {
            const EpdRect partial = {
                .x = (int)resolved.x,
                .y = (int)resolved.y,
                .width = (int)resolved.w,
                .height = (int)resolved.h,
            };
            epd_poweron();
            status = update_and_log(d, MODE_GL16, partial);
            epd_poweroff();
            return status;
        }

        case REFRESH_CLEAR:
            epd_poweron();
            epd_clear();
            epd_poweroff();
            model_glass_as_white(d);
            port_log_write(d->log, LOG_LEVEL_INFO, "adp_display", "clear done", NULL, 0u);
            return HAL_OK;

        case REFRESH_FAST_MONO:
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
    s_hl = epd_hl_init(s_panel->default_waveform);
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
