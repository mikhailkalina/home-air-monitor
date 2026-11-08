// SPDX-License-Identifier: Apache-2.0
//
// How full the battery is, as far as a voltage can say
// (docs/adr/0040-a-battery-gauge-from-the-voltage.md).
//
// This board has no gauge: only a divider from the cell to an ADC input
// (docs/hardware/board_notes.md, "The battery path"). So the percentage is an
// estimate from a table of voltages the composition passes in -- the cell's,
// not the core's -- and it says less on a loaded cell than on a resting one.
// Three things stand between a burst of ADC reads and the number on the
// glass:
//
//   * a median over the last few bursts: the panel, the radio and the SCD41
//     each pull the cell down for moments, and a moment must not read as
//     charge gone. Sags only last a burst or two; the median of five does not
//     see them;
//   * a step: the number shown moves only once the estimate has moved by at
//     least percent_step, or reached either end. On the flat middle of a
//     lithium cell's curve one point is a few millivolts, which is the ADC's
//     noise, and a number trembling there would widen every partial refresh
//     up to the status bar;
//   * a judgement on whether this is a cell at all. Outside what a lithium
//     cell can hold, the voltage is not a state of charge and the screen says
//     "n/a": above it the divider is reading the supply -- USB, with Q3 joining
//     the battery node to the LDO's input while PWR_EN is high -- and below it
//     nothing is connected. With a cell and USB together this cannot tell
//     charging from anything else, and does not try: it shows what the voltage
//     says, which reads high while a charger pushes.
//
// A burst the ADC refuses (Wi-Fi holds ADC2) changes nothing: the number stays
// until no burst has succeeded for max_silence_ms, and then the gauge reports
// an error. A battery's charge moves over hours, so the number's own age is
// the gauge's to judge: its time is the time it is read at, which keeps
// air_reading_apply_age() -- whose limit is the sensors', seconds -- out of it.
//
// Pure: no clock, no allocation, every time and every sample from the caller.

#ifndef HAC_CORE_APP_BATTERY_GAUGE_H
#define HAC_CORE_APP_BATTERY_GAUGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "domain/measurement.h"

#ifdef __cplusplus
extern "C" {
#endif

// How many bursts the median runs over. CHOICE: with
// BATTERY_GAUGE_DEFAULT_SAMPLE_INTERVAL_MS a minute of them, and two of the
// five may be sags without moving the result.
#define BATTERY_GAUGE_WINDOW 5u

// CHOICE: a burst this often. The event loop wakes every few seconds for the
// sensors anyway, so a burst costs no wake of its own.
#define BATTERY_GAUGE_DEFAULT_SAMPLE_INTERVAL_MS 12000u

// ESTIMATE: the number shown moves only once the estimate is this far from it.
// Two points are about 8 mV of the cell on the flat middle of a lithium curve.
// `vbat` saw single reads 11 mV apart at the pin, 22 mV at the cell, beside an
// idle link (2026-10-21); a burst's mean and a median of bursts are quieter by
// an amount nothing has measured. To replace: the first discharge run's log,
// branch phase-5-battery-gauge (docs/hardware/phase-5-bench-plan.md).
#define BATTERY_GAUGE_DEFAULT_PERCENT_STEP 2u

// CHOICE: how far back inside its limits a voltage must come before it counts
// as a cell again, so that a supply resting on a limit does not flap between
// a number and "n/a", each a redraw.
#define BATTERY_GAUGE_DEFAULT_LIMIT_HYSTERESIS_MV 100u

// CHOICE: no good burst for this long and the number is an error. Long enough
// for any burst of Wi-Fi traffic: the board has read ADC2 32 times of 32 beside
// an idle link, and traffic is still to be seen (board_notes.md, open item on
// GPIO14).
#define BATTERY_GAUGE_DEFAULT_MAX_SILENCE_MS 300000u

// One point of the table: at `mv` the cell holds `percent`.
typedef struct {
    uint16_t mv;
    uint8_t percent;
} battery_curve_point_t;

typedef struct {
    // The table, by rising voltage and rising percentage; between two points
    // the percentage is linear, past either end it is that end's. The
    // composition's, and it must outlive the gauge.
    const battery_curve_point_t *curve;
    size_t curve_points;

    // What a cell can hold. Outside these the voltage is not a state of
    // charge; the composition's, as the table is.
    uint16_t lowest_cell_mv;
    uint16_t highest_cell_mv;
    uint16_t limit_hysteresis_mv;

    uint8_t percent_step;
    uint32_t sample_interval_ms;
    uint32_t max_silence_ms;
    uint8_t source_id;  // measurement_f32_t's: which source the numbers carry
} battery_gauge_config_t;

typedef enum {
    BATTERY_GAUGE_NO_SAMPLE = 0,  // nothing read yet
    BATTERY_GAUGE_CELL,           // a cell's voltage, a percentage from it
    BATTERY_GAUGE_NOT_A_CELL,     // a voltage, outside what a cell can hold
    BATTERY_GAUGE_SILENT,         // no good burst for max_silence_ms
} battery_gauge_state_t;

typedef struct {
    battery_gauge_config_t cfg;
    uint16_t window[BATTERY_GAUGE_WINDOW];
    size_t window_count;
    size_t window_next;
    uint16_t median_mv;
    uint8_t shown_percent;
    bool have_shown;
    bool in_limits;           // the median, with the hysteresis, is inside the limits
    bool have_good;           // a good burst since init
    bool have_attempt;        // a burst, good or not, since init
    uint64_t silent_from_ms;  // the last good burst, or the first attempt before one
    uint64_t next_sample_ms;
    bool silent;
} battery_gauge_t;

// The cadence, the step, the hysteresis and the silence above. The table, the
// limits and the source id are left for the composition.
battery_gauge_config_t battery_gauge_config_default(void);

void battery_gauge_init(battery_gauge_t *g, const battery_gauge_config_t *cfg);

// The table at `mv`: linear between two points, the end's past either end, 0
// for an empty table. Rounded to the nearest point.
uint8_t battery_percent_from_mv(const battery_curve_point_t *curve, size_t points, uint32_t mv);

// Whether a burst is due: at once after init, then every sample_interval_ms,
// and at once if the monotonic clock restarted.
bool battery_gauge_due(const battery_gauge_t *g, uint64_t now_ms);

// A burst's outcome: `ok` with the cell's voltage, or refused (`mv` ignored).
void battery_gauge_sample(battery_gauge_t *g, bool ok, uint32_t mv, uint64_t now_ms);

battery_gauge_state_t battery_gauge_state(const battery_gauge_t *g);

// The reading's two fields, at `now_ms`:
//   no sample yet     percent VAL_UNAVAILABLE, mv VAL_UNAVAILABLE
//   a cell            percent VAL_OK,          mv VAL_OK
//   not a cell        percent VAL_UNAVAILABLE, mv VAL_OK: the voltage is real
//   silent            percent VAL_ERROR,       mv VAL_ERROR
void battery_gauge_fill(const battery_gauge_t *g, uint64_t now_ms, measurement_f32_t *percent,
                        measurement_f32_t *mv);

// "no-sample", "cell", "not-a-cell", "silent", for logs; never NULL.
const char *battery_gauge_state_name(battery_gauge_state_t state);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_APP_BATTERY_GAUGE_H
