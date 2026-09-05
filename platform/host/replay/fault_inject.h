// SPDX-License-Identifier: Apache-2.0
//
// Runtime fault injection layered on top of any sensor_source_t. Where
// replay_source's CSV `status` column authors a fault into the timeline
// ahead of time, fault_inject_t is toggled live -- the mechanism behind the
// simulator's FAULTS panel (docs/architecture.md §7.2) and useful directly
// from a test that wants to flip a fault mid-run.
//
// It wraps `inner` and forwards every call unmodified except read(), whose
// behaviour depends on the currently selected fault_kind_t.

#ifndef HAC_PLATFORM_HOST_REPLAY_FAULT_INJECT_H
#define HAC_PLATFORM_HOST_REPLAY_FAULT_INJECT_H

#include <stdbool.h>

#include "domain/measurement.h"
#include "port_sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FAULT_NONE = 0,      // pass every read() through to the inner source unchanged
    FAULT_BUS_NACK,      // read() fails as if the device NACKed: HAL_ERR_IO
    FAULT_BUS_TIMEOUT,   // read() fails as if the bus stopped answering: HAL_ERR_TIMEOUT
    FAULT_STUCK,         // read() succeeds but always returns the last good sample
    FAULT_FORCE_STATUS,  // read() succeeds but every capable field is forced to one status
} fault_kind_t;

typedef struct {
    sensor_source_t port;  // the composed source handed to the sensor manager
    sensor_source_t *inner;

    fault_kind_t kind;
    value_status_t forced_status;  // used only by FAULT_FORCE_STATUS

    sensor_sample_t last_good;
    bool have_last_good;
} fault_inject_t;

// Wraps `inner` (must outlive `fi` and stay unmoved). Starts with FAULT_NONE:
// a freshly created fault_inject_t is transparent.
void fault_inject_init(fault_inject_t *fi, sensor_source_t *inner);

// Selects what read() does from here on. FAULT_FORCE_STATUS additionally
// needs fault_inject_set_forced_status().
void fault_inject_set(fault_inject_t *fi, fault_kind_t kind);
void fault_inject_set_forced_status(fault_inject_t *fi, value_status_t status);
void fault_inject_clear(fault_inject_t *fi);  // == fault_inject_set(fi, FAULT_NONE)

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_REPLAY_FAULT_INJECT_H
