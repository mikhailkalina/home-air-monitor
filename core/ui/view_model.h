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

#ifndef HAC_CORE_UI_VIEW_MODEL_H
#define HAC_CORE_UI_VIEW_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "domain/measurement.h"

#ifdef __cplusplus
extern "C" {
#endif

// Long enough for every string this module produces: "1013" (pressure),
// "-12.3" (temperature), "warm", "n/a", "45 min".
#define VM_TEXT_MAX 12
#define VM_STATUS_TEXT_MAX 32

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
// render as an unannotated blank.
typedef enum {
    VM_MARK_NONE = 0,  // VAL_OK
    VM_MARK_STALE,
    VM_MARK_ERROR,
    VM_MARK_WARMUP,
    VM_MARK_UNAVAILABLE,
} vm_mark_t;

typedef enum {
    VM_LEVEL_UNKNOWN = 0,
    VM_LEVEL_GOOD,
    VM_LEVEL_MODERATE,
    VM_LEVEL_POOR,
    VM_LEVEL_VERY_POOR,
} vm_level_t;

typedef struct {
    const char *label;  // "CO2", "Temperature", ...
    const char *unit;   // "ppm", "C", "%", "hPa", ""; never NULL
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

    bool net_connected;
    const char *net_text;  // "Wi-Fi" / "offline"

    // Age of the freshest quantity that still carries a number, as
    // "12 s" / "4 min" / "2 h". has_updated is false when nothing has ever
    // been read, and updated_text is then "--".
    char updated_text[VM_TEXT_MAX];
    bool has_updated;

    uint8_t error_count;
    uint8_t stale_count;
    uint8_t warmup_count;
    char status_text[VM_STATUS_TEXT_MAX];  // "2 sensor errors", "all sensors OK", ...
} view_model_t;

// Builds the whole view from one reading. `now_ms` comes from port_clock and
// is used only to age the "updated" line; nothing here reads a clock.
void view_model_build(view_model_t *vm, const air_reading_t *reading, uint64_t now_ms);

// The per-status treatments, exposed so that a test can assert them directly
// and so that any later screen renders a status the same way this one does.
vm_mark_t view_model_mark_for(value_status_t status);
uint8_t view_model_ink_for(value_status_t status);

// The CO2 band of docs/requirements.md 13.1. VM_LEVEL_UNKNOWN when the
// measurement carries no number.
vm_level_t view_model_co2_level(const measurement_f32_t *co2);
const char *view_model_level_text(vm_level_t level);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_UI_VIEW_MODEL_H
