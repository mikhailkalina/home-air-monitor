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
// applies to the whole row: ok, error, stale, warmup, nan, out_of_range (see
// replay_row_status_t). Playback loops back to t_offset_s == 0 once past the
// last row; the segment between two consecutive "ok" rows is linearly
// interpolated unless `interpolate` is false or either endpoint is not "ok".
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

// One data field per recognized non-status, non-t_offset_s column.
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
} replay_row_status_t;

typedef struct {
    double t_offset_s;
    double value[REPLAY_FIELD_COUNT];  // meaningless where status != REPLAY_ROW_OK
    replay_row_status_t status;
} replay_row_t;

// The parsed, in-memory form of a scenario file. Rows are heap-allocated
// (rows/row_count/row_capacity) so a file's length is the only limit, up to
// REPLAY_TIMELINE_MAX_ROWS.
typedef struct {
    bool field_present[REPLAY_FIELD_COUNT];  // the column existed in the header
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
// an unrecognized column, a duplicate column), a data row with the wrong
// field count, an unparseable number, or an unrecognized status word.
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

// A source backed by an already-parsed timeline plus the sensor_source_t
// state (start time, playback position) layered on top of it.
typedef struct {
    sensor_source_t port;

    replay_timeline_t timeline;
    const port_clock_t *clock;
    uint64_t start_ms;
    bool started;
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
