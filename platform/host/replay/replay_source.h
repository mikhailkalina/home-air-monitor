// SPDX-License-Identifier: Apache-2.0
//
// A sensor_source_t backed by a CSV scenario file (docs/architecture.md
// §7.1). The header row drives the column mapping, so a scenario file needs
// only the columns it actually has:
//
//   t_offset_s,temperature_c,humidity_rh,pressure_hpa,co2_ppm,gas_res_ohm,status
//   0,22.1,44.0,1013.2,620,180000,ok
//   60,22.2,44.3,1013.1,680,179500,ok
//
// `#` starts a whole-line comment; blank lines are skipped. `t_offset_s` is
// mandatory and must come first; any other recognized column may be omitted,
// in which case every reading for that quantity is VAL_UNAVAILABLE. `status`
// applies to the whole row: ok, error, stale, warmup, unavailable, nan,
// out_of_range (see replay_row_status_t); without the column every row is
// ok. Playback loops back to t_offset_s == 0 once past the last row.
//
// Three more columns are optional, all added in phase 3f for the device's
// recordings (core/recording/record_format.h,
// docs/adr/0024-sd-card-recordings-and-debug-log.md), and a file without them
// parses and plays exactly as before:
//
//   utc              the row's wall time, YYYY-MM-DDTHH:MM:SSZ, or empty
//                    while the recording device's clock was not set.
//                    Playback ignores it; the simulator can start its own
//                    wall clock from the first row's (replay_timeline_t::
//                    first_utc_ms).
//   <field>_status   overrides the row's status for that field alone, so one
//                    row can carry a warming gas reading beside a valid
//                    temperature; empty means the row's.
//   <field>_acc      the source's accuracy verdict for the field
//                    (value_accuracy_t, docs/adr/0021): unreliable, low,
//                    medium, high, or empty for none.
//
// A `<field>_status` or `<field>_acc` column needs its `<field>` column. A
// field's value may be blank wherever that field's status is not ok. The
// segment between two consecutive rows is linearly interpolated, field by
// field, where both rows have that field ok, unless `interpolate` is false;
// accuracy is never interpolated, it is the earlier row's.
//
// A last line with no line end that does not parse is a write cut short --
// a card that lost power, or was pulled, in the middle of a row -- and is
// dropped; so are NUL bytes at the end of the data, which a FAT file can
// carry after a power cut. Anywhere else a malformed line still rejects the
// file.
//
// Each replay_source_t owns its own timeline and its own loop point: two
// sources loaded from files with different durations do not affect each
// other, even when driven from the same port_clock.

#ifndef HAC_PLATFORM_HOST_REPLAY_REPLAY_SOURCE_H
#define HAC_PLATFORM_HOST_REPLAY_REPLAY_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"
#include "port_clock.h"
#include "port_sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

// One data field per recognized value column, in the order of
// core/app/sensor_manager.h's sensor_field_t, whose column names
// (record_column_name()) the parser reads.
typedef enum {
    REPLAY_FIELD_TEMPERATURE_C = 0,
    REPLAY_FIELD_HUMIDITY_RH,
    REPLAY_FIELD_PRESSURE_HPA,
    REPLAY_FIELD_CO2_PPM,
    REPLAY_FIELD_IAQ,
    REPLAY_FIELD_VOC_INDEX,
    REPLAY_FIELD_GAS_RESISTANCE_OHM,
    REPLAY_FIELD_COUNT,
} replay_field_t;

// What the `status` column says about an entire row.
typedef enum {
    REPLAY_ROW_OK = 0,
    REPLAY_ROW_WARMUP,
    REPLAY_ROW_STALE,
    REPLAY_ROW_ERROR,
    REPLAY_ROW_NAN,           // exercises measurement_ok()'s own NaN rejection
    REPLAY_ROW_OUT_OF_RANGE,  // a physically implausible value; also VAL_ERROR
    REPLAY_ROW_UNAVAILABLE,   // the source did not provide it this time
} replay_row_status_t;

typedef struct {
    double t_offset_s;
    double value[REPLAY_FIELD_COUNT];  // meaningless where field_status != REPLAY_ROW_OK
    replay_row_status_t status;        // the `status` column
    // Per field: the row's status, or the field's own `<field>_status`.
    replay_row_status_t field_status[REPLAY_FIELD_COUNT];
    value_accuracy_t accuracy[REPLAY_FIELD_COUNT];  // ACC_NOT_REPORTED without `<field>_acc`
    uint64_t utc_ms;                                // 0: no `utc` column, or an empty cell
} replay_row_t;

// The parsed, in-memory form of a scenario file. Rows are heap-allocated
// (rows/row_count/row_capacity) so a file's length is the only limit, up to
// REPLAY_TIMELINE_MAX_ROWS.
typedef struct {
    bool field_present[REPLAY_FIELD_COUNT];     // the column existed in the header
    bool accuracy_present[REPLAY_FIELD_COUNT];  // so did `<field>_acc`
    bool utc_present;                           // so did `utc`
    uint64_t first_utc_ms;                      // the first row's `utc`; 0 for none
    replay_row_t *rows;
    size_t row_count;
    size_t row_capacity;
    double loop_period_s;  // t_offset_s of the last row; 0 when row_count <= 1
} replay_timeline_t;

#define REPLAY_TIMELINE_MAX_ROWS 65536u

// Parses CSV text held in memory (not required to be NUL-terminated). This is
// the pure half of the loader: no filesystem access, safe to call on
// arbitrary bytes (the fuzz target in tests/fuzz calls it directly).
//
// Returns HAL_ERR_INVALID_ARG for a missing/malformed header (no t_offset_s,
// an unrecognized column, a duplicate column, a `<field>_status` or
// `<field>_acc` without its `<field>`), a data row with the wrong field
// count, an unparseable number or `utc`, or an unrecognized status or
// accuracy word.
// Returns HAL_ERR_NO_MEM if a row fails to allocate or the file holds more
// than REPLAY_TIMELINE_MAX_ROWS data rows. On any error, `*out` is left
// zeroed (nothing to dispose).
hal_status_t replay_parse_buffer(const char *data, size_t len, replay_timeline_t *out);

// Reads the whole file at `path`, then parses it via replay_parse_buffer().
// Returns HAL_ERR_NOT_FOUND if the file cannot be opened, HAL_ERR_NO_MEM if
// reading it into memory fails.
hal_status_t replay_parse_file(const char *path, replay_timeline_t *out);

// Frees the row array. Safe to call on a zeroed or already-disposed timeline.
void replay_timeline_dispose(replay_timeline_t *timeline);

// The capabilities the file's value columns show: the SENS_CAP_* bit of each
// field present. What a recording replays with (apps/simulator --recording).
sensor_caps_t replay_timeline_caps(const replay_timeline_t *tl);

// The fields with an accuracy column.
sensor_caps_t replay_timeline_accuracy_caps(const replay_timeline_t *tl);

// The scenario `elapsed_ms` after its start, looping as playback does: each
// field as a replay source reports it -- the value of a row where the field
// is "ok" (interpolated towards the next row when that field is "ok" there
// too and `interpolate` is set), the field's status otherwise,
// VAL_UNAVAILABLE for a column the file lacks, with the row's accuracy --
// stamped `ts_ms` and `id`. What replay_source's read() returns, and what
// the fake bus's sensor models measure in the simulator's fake-i2c mode
// (platform/host/scenario_world.h).
void replay_timeline_sample(const replay_timeline_t *tl, uint64_t elapsed_ms, bool interpolate,
                            uint64_t ts_ms, uint8_t id, sensor_sample_t *out);

// A source backed by an already-parsed timeline plus the sensor_source_t
// state (start time, playback position) layered on top of it.
typedef struct {
    sensor_source_t port;

    replay_timeline_t timeline;
    const port_clock_t *clock;
    uint64_t start_ms;
    bool started;    // the scenario's clock runs from start_ms
    bool measuring;  // start() since the last init(): read() returns data
    bool interpolate;
    char name[64];
} replay_source_t;

// Loads `path` and wires up `out->port` as a sensor_source_t: `caps` becomes
// out->port.caps (normally the OR of the SENS_CAP_* bits matching the
// columns actually present, but the caller may narrow or widen it), `id`
// becomes source_id on every measurement this source produces. Interpolation
// is linear between consecutive "ok" rows unless `interpolate` is false, in
// which case each row's value holds (a step function) until the next row --
// closer to how an abruptly faulting sensor behaves.
//
// `out` must stay valid and unmoved for as long as `out->port` is in use;
// `clock` must outlive it too.
hal_status_t replay_source_init(replay_source_t *out, const char *path, sensor_caps_t caps,
                                uint8_t id, const port_clock_t *clock, bool interpolate);

void replay_source_deinit(replay_source_t *rs);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_REPLAY_REPLAY_SOURCE_H
