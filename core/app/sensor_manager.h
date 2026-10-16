// SPDX-License-Identifier: Apache-2.0
//
// The sensor manager of docs/architecture.md 6.3: it drives every
// sensor_source_t through its life cycle (ports/port_sensor.h), keeps each
// source's latest sample, and merges them into the sensor fields of one
// air_reading_t, a field at a time, by a priority the composition gives it.
//
// What it promises (docs/requirements.md 15.1, docs/adr/0017-sensor-manager.md):
//
//   * a failing source never stops the others. Each source has its own
//     schedule, and nothing here waits: init() is the only call that may take
//     a while, and only as long as the driver's own power-up and reset times.
//   * a failure is visible. The failing source's fields become VAL_ERROR at
//     once; where the priority names another source for that field, the
//     merge takes that one instead, and gives the field back when the first
//     recovers.
//   * a source that keeps failing is started over. After
//     failures_before_reinit failures in a row -- a read refused, a start
//     refused, or a measurement that never arrives -- the manager calls its
//     init() again, after a backoff that doubles up to a ceiling, so an
//     unplugged sensor costs one attempt per backoff period and is found
//     again when it is plugged back, without a reboot.
//   * a source that corrects its readings for conditions measured elsewhere
//     (sensor_source_t::set_ambient, docs/adr/0018) gets them handed over:
//     after it is initialised, and again whenever they have moved by more
//     than a threshold.
//
// Which source is which is not written here. The composition registers the
// sources with their ids and says, per field, which ids to prefer
// (sensor_manager_set_priority()); a field with no priority takes the sources
// in the order they were added. The core knows that temperature may come
// from two places, not that one of them is a BME68x.
//
// Like update_policy, the module is pure: no allocation, no stdio, and every
// "now" from the port_clock handed to sensor_manager_init(). It logs state
// changes -- a source failing, recovering, being started over -- through
// port_log, once per change rather than once per poll.

#ifndef HAC_CORE_APP_SENSOR_MANAGER_H
#define HAC_CORE_APP_SENSOR_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "domain/measurement.h"
#include "hal_status.h"
#include "port_clock.h"
#include "port_log.h"
#include "port_sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

// The board has two sensors; four leaves room for one more and a stand-in.
#define SENSOR_MANAGER_MAX_SOURCES 4u

// Returned by sensor_manager_poll() when no source is registered.
#define SENSOR_MANAGER_NO_DEADLINE UINT64_MAX

// CHOICE: how often a source is asked for a measurement unless its
// composition says otherwise. Requirements 11.2 asks for a sensor read every
// 5 to 30 s on USB power; 5 s is the SCD4x's own periodic interval (its
// datasheet, 3.6.1), so nothing is read faster than the CO2 sensor produces.
#define SENSOR_MANAGER_DEFAULT_INTERVAL_MS 5000u

// CHOICE: a reading older than this is VAL_STALE (requirements 9.2), and
// loses the merge to a fresh one from a lower-priority source. Six default
// intervals: one missed read is not an alarm, a stopped sensor is within half
// a minute.
#define SENSOR_MANAGER_DEFAULT_MAX_AGE_MS 30000u

// CHOICE: a measurement still not delivered this long after the cycle began
// is a failure, though the source only ever said "not yet" -- a sensor that
// stopped producing data is otherwise never started over. As long as a
// reading stays fresh, so the stalled source's fields go from VAL_STALE to
// VAL_ERROR rather than straight from VAL_OK.
#define SENSOR_MANAGER_DEFAULT_STALL_TIMEOUT_MS 30000u

// CHOICE: failures in a row before the source is initialised again. One
// glitch on a shared bus is retried at the next interval; the third in a row
// means the sensor is not what the driver thinks it is any more.
#define SENSOR_MANAGER_DEFAULT_FAILURES_BEFORE_REINIT 3u

// CHOICE: the backoff before a re-init, doubling from the first to the
// ceiling for as long as no reading succeeds. The ceiling trades two costs
// against each other (docs/adr/0017-sensor-manager.md): how long a sensor
// plugged back in waits to be found, and what an absent one costs -- on the
// board, each attempt at an absent sensor is a few failed transfers and the
// ESP-IDF I2C driver's error lines for each. At 120 s an absent sensor is
// tried 30 times an hour and a replugged one is back within two minutes.
#define SENSOR_MANAGER_DEFAULT_BACKOFF_FIRST_MS 5000u
#define SENSOR_MANAGER_DEFAULT_BACKOFF_MAX_MS 120000u

// CHOICE: the soonest a source that said "not yet" (HAL_ERR_NOT_READY from
// read(), HAL_ERR_BUSY from start()) is asked again when its ready_in_ms()
// does not name a later time. A floor, so a source that answers "not yet"
// with ready_in_ms() == 0 cannot make the caller spin.
#define SENSOR_MANAGER_DEFAULT_RETRY_MS 100u

// CHOICE: how far the merged pressure must move before it is handed to the
// sources that compensate for it again. The SCD4x takes it in whole hPa (its
// datasheet, 3.7.5), so a smaller step changes nothing it computes.
#define SENSOR_MANAGER_DEFAULT_PRESSURE_DELTA_HPA 1.0f

// The quantities of sensor_sample_t, for the priorities.
typedef enum {
    SENSOR_FIELD_TEMPERATURE = 0,
    SENSOR_FIELD_HUMIDITY,
    SENSOR_FIELD_PRESSURE,
    SENSOR_FIELD_CO2,
    SENSOR_FIELD_IAQ,
    SENSOR_FIELD_VOC_INDEX,
    SENSOR_FIELD_GAS_RESISTANCE,
    SENSOR_FIELD_COUNT,
} sensor_field_t;

typedef struct {
    uint32_t max_age_ms;
    uint32_t stall_timeout_ms;
    uint32_t failures_before_reinit;  // at least 1
    uint32_t backoff_first_ms;
    uint32_t backoff_max_ms;
    uint32_t retry_ms;
    float pressure_delta_hpa;
} sensor_manager_config_t;

// Where one source is in its life cycle.
typedef enum {
    SENSOR_SLOT_UNINIT = 0,  // init() not tried yet
    SENSOR_SLOT_IDLE,        // initialised; the next start() is due at due_ms
    SENSOR_SLOT_MEASURING,   // started; read() is due at due_ms
    SENSOR_SLOT_BACKOFF,     // failing; init() again at due_ms
} sensor_slot_state_t;

// One registered source and what the manager knows about it. Public so a
// composition can log it and a test can inspect it; written only by this
// module.
typedef struct {
    sensor_source_t *source;
    uint32_t interval_ms;

    sensor_slot_state_t state;
    uint64_t due_ms;            // when this slot next needs a call
    uint64_t next_start_ms;     // the next cycle's start, interval_ms after this one's
    uint64_t cycle_started_ms;  // when the measurement now awaited was first asked for
    bool in_cycle;              // cycle_started_ms is set

    uint32_t consecutive_failures;  // since the last successful read
    uint32_t reinit_attempts;       // re-inits since the last successful read
    bool failing;                   // a failure was logged and no read succeeded since
    bool ever_ready;                // init() has succeeded at least once

    // The latest sample, restricted to the source's capabilities, with every
    // field VAL_ERROR after a failure.
    sensor_sample_t sample;

    // What set_ambient() was last handed since the last init().
    bool ambient_sent;
    float ambient_pressure_hpa;

    // Counters, for tests and logs.
    uint32_t inits;  // init() calls, successful or not
    uint32_t reads;  // successful reads
    uint32_t failures;
} sensor_manager_slot_t;

typedef struct {
    const port_clock_t *clock;
    const port_log_t *log;  // may be NULL
    sensor_manager_config_t cfg;

    sensor_manager_slot_t slots[SENSOR_MANAGER_MAX_SOURCES];
    size_t count;

    // Per field: source ids, most preferred first; 0 ends the list.
    uint8_t priority[SENSOR_FIELD_COUNT][SENSOR_MANAGER_MAX_SOURCES];

    uint64_t next_deadline_ms;
} sensor_manager_t;

// The defaults above.
sensor_manager_config_t sensor_manager_config_default(void);

// An empty manager. `clock` and `cfg` are required (the clock is kept by
// pointer, so it must outlive the manager); `log` may be NULL.
void sensor_manager_init(sensor_manager_t *m, const port_clock_t *clock, const port_log_t *log,
                         const sensor_manager_config_t *cfg);

// Registers `source`, to be measured every `interval_ms`. Nothing is called on
// it yet: the first sensor_manager_poll() initialises it. The source must
// outlive the manager. HAL_ERR_INVALID_ARG for a NULL source, an id of 0 (it
// means "no source" in a measurement), an id already registered, or an
// interval of 0; HAL_ERR_NO_MEM when SENSOR_MANAGER_MAX_SOURCES are already
// registered.
hal_status_t sensor_manager_add(sensor_manager_t *m, sensor_source_t *source, uint32_t interval_ms);

// Which sources to take `field` from, most preferred first: `count` source
// ids, at most SENSOR_MANAGER_MAX_SOURCES. An id not registered, or whose
// source lacks the capability, is skipped when the merge runs, so the order
// may name a source added later. HAL_ERR_INVALID_ARG for an id of 0, a field
// out of range or too many ids.
hal_status_t sensor_manager_set_priority(sensor_manager_t *m, sensor_field_t field,
                                         const uint8_t *ids, size_t count);

// Calls whatever is due -- init(), start(), read(), set_ambient() -- then
// writes the merged sensor fields (temperature, humidity, pressure, CO2, IAQ,
// VOC index, gas resistance) into `reading`, leaving its other fields alone.
// Returns the point on the clock at which it next needs to be called, and
// never one that has already passed: the caller waits until then, or until
// something else wakes it. Calling earlier is harmless.
//
// The call takes time -- the drivers wait out their sensors' command times
// on the clock -- so the fields it writes are stamped later than the moment
// it was called. A caller that ages the reading afterwards
// (air_reading_apply_age()) must read the clock again first: against the
// earlier time every fresh value would be "from the future", of unknown age,
// and stale.
//
// The merge, per field: among the sources the priority names (or all, in the
// order added), the one whose value is best -- VAL_OK, then VAL_STALE (aged
// with max_age_ms), then VAL_WARMUP, then VAL_ERROR, then VAL_UNAVAILABLE --
// and of equals the one named first.
uint64_t sensor_manager_poll(sensor_manager_t *m, air_reading_t *reading);

// What sensor_manager_poll() last returned.
uint64_t sensor_manager_next_deadline(const sensor_manager_t *m);

// The slot registered `index`-th, or NULL past the last.
const sensor_manager_slot_t *sensor_manager_slot(const sensor_manager_t *m, size_t index);

// Stable, allocation-free names for logs. Never NULL.
const char *sensor_manager_slot_state_name(sensor_slot_state_t state);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_APP_SENSOR_MANAGER_H
