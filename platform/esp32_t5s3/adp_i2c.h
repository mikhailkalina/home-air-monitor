// SPDX-License-Identifier: Apache-2.0
//
// port_i2c over ESP-IDF 5.5's i2c_master driver (driver/i2c_master.h -- the
// new driver, not the legacy driver/i2c.h), on the board's one shared bus:
// BOARD_I2C_SDA_GPIO / BOARD_I2C_SCL_GPIO, BOARD_I2C_SCL_HZ, the S3's I2C
// controller 0. docs/adr/0011-i2c-bus-adapter.md records the design; the
// short version:
//
//   * No ESP-IDF type leaves this adapter. Driver handles are held as
//     `void *` below, and every esp_err_t is mapped onto hal_status_t inside
//     adp_i2c.c.
//   * Every call's `to_ms` becomes one absolute deadline, and every wait
//     inside the call -- for the bus lock, for the driver, for the follow-up
//     probe -- gets only what is left of it, converted so the driver's
//     tick-truncating arithmetic cannot shorten it (adp_i2c_pure.h).
//   * The driver reports every failed transfer as ESP_ERR_INVALID_STATE, so
//     a failure is classified by probing the same address once more
//     (adp_i2c_pure.h). That probe is the only extra bus traffic this adapter
//     ever generates, and only on a failure.
//   * recover() bit-bangs the bus-clear procedure on the two pins as plain
//     GPIOs -- the controller is torn down first and rebuilt afterwards -- so
//     the nine clocks are this file's, observable and counted, rather than
//     the controller's clear-bus feature, which the driver runs only when its
//     own state says so and never reports on (see the ADR).
//
// Thread-safe: the console task (the i2c_scan command) and, from phase 3, the
// event loop both reach this bus, so every call holds a mutex for its
// duration -- waiting for it out of the same deadline.

#ifndef HAC_PLATFORM_ESP32_T5S3_ADP_I2C_H
#define HAC_PLATFORM_ESP32_T5S3_ADP_I2C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"
#include "port_i2c.h"
#include "port_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// Driver device handles kept open, one per address talked to. A ninth
// address evicts the oldest handle -- a malloc and a free, not an error.
// The board carries an RTC and a touch controller and phase 3 adds two
// sensors, so in normal use nothing is ever evicted.
#define ADP_I2C_MAX_DEVICES 8u

typedef struct {
    uint8_t addr;
    void *handle;  // i2c_master_dev_handle_t
} adp_i2c_device_t;

typedef struct {
    port_i2c_t port;
    const port_log_t *log;  // borrowed; may be NULL

    void *bus;   // i2c_master_bus_handle_t; NULL if the last rebuild failed
    void *lock;  // SemaphoreHandle_t (a FreeRTOS mutex)

    adp_i2c_device_t devices[ADP_I2C_MAX_DEVICES];
    size_t device_count;
    size_t next_eviction;

    // Set when a transfer ends in HAL_ERR_TIMEOUT or HAL_ERR_IO; the next
    // transfer resets the controller first. See adp_i2c.c, transfer().
    bool reset_pending;

    // What the most recent recover() did, for the log and the console.
    unsigned last_recover_pulses;
} adp_i2c_t;

// Brings the bus up and logs the line levels it finds: an idle bus reads high
// on both. If it fails, every later call returns an error rather than
// misbehaving: HAL_ERR_NOT_READY if not even the adapter's mutex could be
// created, HAL_ERR_IO from transfers if the controller could not be, until a
// recover() rebuilds it. The S3's internal pull-ups are enabled -- harmless beside any
// external ones, and this project has not confirmed from a schematic that
// external ones exist. `log` may be NULL.
hal_status_t adp_i2c_init(adp_i2c_t *a, const port_log_t *log);

port_i2c_t *adp_i2c_port(adp_i2c_t *a);

void adp_i2c_deinit(adp_i2c_t *a);

// The two lines as the pins read them right now, whichever owns them. An
// idle bus is high on both; SDA low with SCL high between transfers is a
// slave stuck mid-byte.
void adp_i2c_lines(adp_i2c_t *a, bool *sda_high, bool *scl_high);

// --- fault injection: tests/contract/esp32/test_port_i2c_contract.c only ---
//
// Compiled into every build and called from none but the test app's; the
// linker drops them from the firmware image, as main/epd_calibration.c's
// unused mode is dropped.

// Leaves the bus as a reset in the middle of a read would: bit-bangs START
// and `addr` with the read bit, clocks the slave's acknowledge onto SDA, and
// stops with SCL released high -- so the slave keeps SDA low, waiting for a
// falling edge that never comes. Then hands the pins back to a rebuilt
// controller. `addr` must be a device that is present. HAL_OK only if the
// slave acknowledged and SDA still reads low once the controller owns the
// pins again.
hal_status_t adp_i2c_inject_stuck_slave(adp_i2c_t *a, uint8_t addr);

// Takes SCL away from the controller and drives it low, while the
// controller keeps reading it: what the controller sees is a slave
// stretching the clock forever. adp_i2c_release_scl() hands SCL back to the
// controller without rebuilding it -- the slave lets go -- so whatever state
// the stall left the controller in, the next transfer meets it.
hal_status_t adp_i2c_hold_scl_low(adp_i2c_t *a);
hal_status_t adp_i2c_release_scl(adp_i2c_t *a);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_ADP_I2C_H
