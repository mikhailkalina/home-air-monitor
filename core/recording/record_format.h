// SPDX-License-Identifier: Apache-2.0
//
// One sensor source's recording, as text: the scenario format of
// docs/architecture.md 7.1 that the simulator replays
// (platform/host/replay/replay_source.h), with the optional columns phase 3f
// added to it (docs/adr/0024-sd-card-recordings-and-debug-log.md):
//
//   # home-air-monitor recording: source bme68x, id 2
//   t_offset_s,utc,temperature_c,humidity_rh,pressure_hpa,iaq,gas_res_ohm,status,temperature_c_status,...,iaq_acc
//   0.000,2026-10-16T12:00:03Z,23.41,48.20,1006.214,,153204,ok,,,,warmup,,unreliable
//
//   t_offset_s       seconds since the file's first row, to the millisecond
//   utc              the row's wall time, YYYY-MM-DDTHH:MM:SSZ; empty while
//                    the clock is not set (docs/adr/0023)
//   <field>          the value, with the field's fixed number of decimals,
//                    for a status that carries one (ok, stale); else empty
//   status           the first field's status word
//   <field>_status   the field's status word where it differs from `status`;
//                    else empty
//   <field>_acc      the source's own accuracy verdict (docs/adr/0021):
//                    unreliable, low, medium, high, or empty for none --
//                    only for the fields the composition says carry one
//
// Everything here is pure: the caller owns the buffer, nothing is allocated,
// and a row that does not fit is not written at all (0 is returned).

#ifndef HAC_CORE_RECORDING_RECORD_FORMAT_H
#define HAC_CORE_RECORDING_RECORD_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/sensor_manager.h"
#include "domain/measurement.h"
#include "port_sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

// "YYYY-MM-DDTHH:MM:SSZ" and "YYYY-MM-DDTHH:MM:SS.mmmZ", without terminator.
#define RECORD_UTC_LEN 20u
#define RECORD_UTC_MS_LEN 24u

// Room for any header or row this module writes: 3 + 7 x 3 columns, none
// wider than 24 characters.
#define RECORD_LINE_MAX 512u

// The column of a field's value, as the replay parser reads it:
// "temperature_c", "humidity_rh", "pressure_hpa", "co2_ppm", "iaq",
// "voc_index", "gas_res_ohm". NULL for a field out of range.
const char *record_column_name(sensor_field_t field);

// How many decimals a field's values are written with (a CHOICE each, the
// sensors' resolution or better: record_format.c).
unsigned record_decimals(sensor_field_t field);

// The SENS_CAP_* bit that claims `field`.
sensor_caps_t record_field_cap(sensor_field_t field);

// "ok", "warmup", "stale", "error", "unavailable".
const char *record_status_word(value_status_t status);

// "" (not reported), "unreliable", "low", "medium", "high".
const char *record_accuracy_word(value_accuracy_t accuracy);

// The inverse of record_accuracy_word() for the `len` bytes at `word`; the
// empty word is ACC_NOT_REPORTED. False for anything else.
bool record_accuracy_from_word(const char *word, size_t len, value_accuracy_t *out);

// The UTC time of `unix_ms` -- to the second, or with milliseconds -- at
// `len` in `dst` (text_append.h's conventions).
size_t record_format_utc(char *dst, size_t cap, size_t len, uint64_t unix_ms, bool with_ms);

// The comment line a recording starts with, naming the source.
size_t record_preamble(char *dst, size_t cap, const char *tag, uint8_t source_id);

// The column header for a source claiming `caps`, with an accuracy column
// for each field in `accuracy_caps` & `caps`, and its newline.
size_t record_header(char *dst, size_t cap, sensor_caps_t caps, sensor_caps_t accuracy_caps);

// One row, and its newline, for the claimed fields of `sample`.
// `t_offset_ms` is the time since the file's first row; `utc_ms` the row's
// wall time, 0 for none.
size_t record_row(char *dst, size_t cap, const sensor_sample_t *sample, sensor_caps_t caps,
                  sensor_caps_t accuracy_caps, uint64_t t_offset_ms, uint64_t utc_ms);

// When the sample was taken: the latest acquisition time among the fields
// `caps` claims, 0 when none carries one.
uint64_t record_sample_time_ms(const sensor_sample_t *sample, sensor_caps_t caps);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_RECORDING_RECORD_FORMAT_H
