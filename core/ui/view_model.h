// SPDX-License-Identifier: Apache-2.0
//
// air_reading_t turned into exactly what the home screen draws: strings,
// marks and grey levels, and nothing that still needs a decision.
//
// The split exists so that "what does the user see when the SCD41 fails" is a
// unit test over plain data rather than a pixel comparison. screen_home.c does
// no interpretation of its own: if a rule is not visible here, the screen does
// not apply it.
//
// Every value_status_t reaches the glass as a distinct treatment -- distinct
// text, a distinct mark and a distinct ink -- so that a field is never
// silently blank (docs/requirements.md 9.2, 14.4, 15.1):
//
//   VAL_OK           the number                black,       no mark
//   VAL_STALE        the number, still shown   mid grey,    clock mark
//   VAL_ERROR        "ERR"                     black,       cross mark
//   VAL_WARMUP       "warm"                    light grey,  hourglass mark
//   VAL_UNAVAILABLE  "n/a"                     lighter grey, dashes mark
//
// A VAL_OK number whose source rates it ACC_LOW or ACC_UNRELIABLE
// (measurement_low_accuracy()) keeps its black ink and gets a question mark
// instead of no mark: a reading, on a scale its source has not yet
// calibrated. Any other status keeps its own mark, which says more. The
// footer names it when nothing worse is wrong, "IAQ not calibrated", or
// counts them, "2 not calibrated"
// (docs/adr/0021-measurement-accuracy-and-iaq-levels.md; the shorter forms
// since docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md).
//
// The IAQ cell. An index of air quality needs Bosch's BSEC library, which a
// build links only on request (docs/adr/0020-bsec-iaq.md); without it the
// BME68x still measures its gas sensor's raw resistance, which
// docs/requirements.md 17.2 asks to be shown in its place. So when IAQ is
// VAL_UNAVAILABLE -- nothing provides it -- and gas resistance is anything
// else, the cell shows gas resistance, in kOhm, under its own label and
// never as IAQ (13.3), with the gas's own status treatment. An IAQ in any
// other status keeps the cell: warming up or failing, it is provided. The
// "updated" line and the fault counts follow whichever the cell shows.
//
// The "updated" line says when the values on the screen were measured, as a
// local time of day: "HH:MM", the minute the freshest value carrying a number
// was measured, rounded down; "DD.MM HH:MM" when that was on an earlier local
// day than the reading's own wall time; "--:--" while no wall time is known
// (air_reading_t.wall_time_ms is 0) or nothing carries a number. Never an
// age: an age is out of date the moment the e-paper keeps showing it, and
// would need a redraw on a timer to stay true. A time of measurement stays
// true for as long as the readings it describes are the ones on the glass --
// which, with nothing redrawn because time passed, is exactly as long as it
// is shown. Freshness is the stale marks' job
// (docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md).
//
// Under an IAQ number, where the other cells have their unit, goes its band
// (core/domain/air_quality.h), in a word that fits the cell at 32px beside a
// mark: "Excellent", "Good", "Fair" for lightly polluted -- Bosch's own
// "Lightly polluted" is half again too wide -- "Moderate" and "Poor".
//
// The status bar's link (docs/adr/0031-the-network-on-the-screen.md) says
// where the readings go, in the held state the reading carries
// (core/app/link_hold.h): "online" -- an MQTT session, the readings
// published, Home Assistant's word for the device being there -- in black;
// "no broker" -- the Wi-Fi up, no session -- and "no Wi-Fi" in the stale
// grey, a weaker claim, as before; and nothing at all without telemetry
// (LINK_OFF) or before a state has held (LINK_UNKNOWN). The words replace
// requirements 9.2's "Connected / Offline", which cannot tell the two ways of
// being offline apart.

#ifndef HAC_CORE_UI_VIEW_MODEL_H
#define HAC_CORE_UI_VIEW_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "domain/air_quality.h"
#include "domain/civil_time.h"
#include "domain/measurement.h"

#ifdef __cplusplus
extern "C" {
#endif

// Long enough for every reading this module produces: "1013" (pressure),
// "-12.3" (temperature), "warm", "n/a".
#define VM_TEXT_MAX 12
#define VM_STATUS_TEXT_MAX 32
// "DD.MM HH:MM", the longest "updated" text.
#define VM_UPDATED_TEXT_MAX 12

// Grey levels in the PIXFMT_GRAY4 range, 0 = black, 15 = white. The screen
// draws on a white ground, so a lighter ink reads as a weaker claim: a stale
// number is visibly fainter than a fresh one before anything is read.
#define VM_INK_PRIMARY 0u
#define VM_INK_SECONDARY 4u
#define VM_INK_STALE 6u
#define VM_INK_WARMUP 9u
#define VM_INK_UNAVAILABLE 11u
#define VM_INK_BACKGROUND 15u

// The CO2 bands of docs/requirements.md 13.1.
#define VM_CO2_GOOD_MAX_PPM 800.0f
#define VM_CO2_MODERATE_MAX_PPM 1200.0f
#define VM_CO2_POOR_MAX_PPM 2000.0f

// Gas resistance in the IAQ cell is shown in kOhm: with one decimal below
// 10 kOhm, where a whole number would drop most of what is left of the
// signal, and whole numbers from there up. The switch sits at 9.95, so the
// decision follows how the value rounds: 9.94 reads "9.9", 9.96 reads "10",
// never "10.0".
#define VM_OHM_PER_KOHM 1000.0f
#define VM_GAS_ONE_DECIMAL_BELOW_KOHM 9.95f

// The quantities the MVP screen shows (docs/requirements.md 14.4). Battery is
// carried separately: it belongs to the status bar, not the reading grid.
typedef enum {
    VM_FIELD_CO2 = 0,
    VM_FIELD_TEMPERATURE,
    VM_FIELD_HUMIDITY,
    VM_FIELD_PRESSURE,
    VM_FIELD_IAQ,
    VM_FIELD_COUNT,
} vm_field_t;

// The icon drawn beside a field. One per value_status_t, so no status can
// render as an unannotated blank, and one for a VAL_OK number its source has
// not calibrated.
typedef enum {
    VM_MARK_NONE = 0,  // VAL_OK
    VM_MARK_STALE,
    VM_MARK_ERROR,
    VM_MARK_WARMUP,
    VM_MARK_UNAVAILABLE,
    VM_MARK_LOW_ACCURACY,  // VAL_OK, measurement_low_accuracy()
} vm_mark_t;

typedef enum {
    VM_LEVEL_UNKNOWN = 0,
    VM_LEVEL_GOOD,
    VM_LEVEL_MODERATE,
    VM_LEVEL_POOR,
    VM_LEVEL_VERY_POOR,
} vm_level_t;

typedef struct {
    const char *label;  // "CO2", "Temperature", ..., "IAQ" or "Gas"
    // The line under the number: its unit -- "ppm", "C", "%", "hPa", "kOhm" --
    // or, for an IAQ number, its band ("Good"); "" for an IAQ without one.
    // Never NULL.
    const char *unit;
    char text[VM_TEXT_MAX];
    value_status_t status;
    vm_mark_t mark;
    uint8_t ink;
    bool has_value;  // false when `text` is a placeholder rather than a number
} vm_field_view_t;

typedef struct {
    vm_field_view_t fields[VM_FIELD_COUNT];
    vm_field_view_t battery;

    // Derived from CO2 only. The IAQ field carries its own number; this band
    // is what drives the headline and the high-CO2 warning of 9.2.
    vm_level_t co2_level;
    const char *co2_level_text;  // "Good", "Moderate", "Poor", "Very poor", "--"
    bool co2_warning;            // VM_LEVEL_POOR or worse

    // The IAQ's band, IAQ_LEVEL_UNKNOWN while the cell shows no IAQ number --
    // gas resistance in its place included. Its text is the IAQ field's unit.
    iaq_level_t iaq_level;

    // The link (above): the held state, its word -- "" for LINK_OFF and
    // LINK_UNKNOWN -- and its ink. link_slot is false only without
    // telemetry: the bar then keeps no place for a word.
    link_status_t link;
    const char *link_text;
    uint8_t link_ink;
    bool link_slot;

    // When the screen's data was measured, in local time: "14:07", or
    // "15.10 23:58" from an earlier day; "--:--", with has_updated false,
    // when that is not known (above, "The updated line").
    char updated_text[VM_UPDATED_TEXT_MAX];
    bool has_updated;

    uint8_t error_count;
    uint8_t stale_count;
    uint8_t warmup_count;
    uint8_t low_accuracy_count;            // VAL_OK numbers with VM_MARK_LOW_ACCURACY
    char status_text[VM_STATUS_TEXT_MAX];  // "2 sensor errors", "all sensors OK", ...
} view_model_t;

// Builds the whole view from one reading. `now_ms` is port_clock's monotonic
// time at the moment `reading->wall_time_ms` was read -- both loops read the
// two together, after the poll -- so that a value measured at ts_ms was
// measured at wall_time_ms - (now_ms - ts_ms). `zone` turns that into the
// local time the "updated" line shows; it is the composition's
// (core/domain/civil_time.h), and required. Nothing here reads a clock.
void view_model_build(view_model_t *vm, const air_reading_t *reading, uint64_t now_ms,
                      const time_zone_t *zone);

// The per-status treatments, exposed so that a test can assert them directly
// and so that any later screen renders a status the same way this one does.
vm_mark_t view_model_mark_for(value_status_t status);
uint8_t view_model_ink_for(value_status_t status);

// The CO2 band of docs/requirements.md 13.1. VM_LEVEL_UNKNOWN when the
// measurement carries no number.
vm_level_t view_model_co2_level(const measurement_f32_t *co2);
const char *view_model_level_text(vm_level_t level);

// What the IAQ cell says under its number: "Excellent", "Good", "Fair",
// "Moderate", "Poor", and "" for IAQ_LEVEL_UNKNOWN.
const char *view_model_iaq_level_text(iaq_level_t level);

// The status bar's word for a link: "online", "no broker", "no Wi-Fi", and
// "" for LINK_OFF and LINK_UNKNOWN. The bar keeps a place as wide as the
// widest of them, so a change of word moves nothing else.
const char *view_model_link_text(link_status_t link);

// A local time of day as the screens write it: "HH:MM", or "DD.MM HH:MM"
// when `at_wall_ms` falls on an earlier local day than `now_wall_ms`; both
// UTC milliseconds, `zone` the composition's. Into `dst`, terminated, cut
// short if `cap` is smaller than VM_UPDATED_TEXT_MAX.
void view_model_local_time_text(char *dst, size_t cap, uint64_t at_wall_ms, uint64_t now_wall_ms,
                                const time_zone_t *zone);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_UI_VIEW_MODEL_H
