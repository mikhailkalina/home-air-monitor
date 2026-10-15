// SPDX-License-Identifier: Apache-2.0
//
// Sensirion's I2C HAL (vendor/sensirion_i2c_hal.h) implemented over port_i2c
// and port_clock: the glue that lets Sensirion's own SCD4x code run
// unchanged on the board (over adp_i2c) and on a PC (over adp_i2c_fake and
// its SCD41 model).
//
// ONE SCD4x PER IMAGE. Sensirion's HAL takes no context: sensirion_i2c_hal_
// read(), _write() and _sleep_usec() are free functions, so the bus and the
// clock they reach are this file's globals, set by scd4x_hal_claim().
// vendor/scd4x_i2c.c keeps two globals of its own besides -- the address
// from scd4x_init() and a shared command buffer. A second SCD4x could not
// share this bus anyway (0x62 is its only address); on a second bus it would
// need sensirion_i2c_hal_select_bus() and a lock around every vendor call.
// Nothing on this board calls for that. The claim makes a second user an
// error rather than a silent takeover of the first one's sensor.
//
// Errors. Sensirion's code passes the HAL's return value through unchanged,
// as an int16_t that cannot carry a hal_status_t. So the HAL returns
// Sensirion's own codes (I2C_NACK_ERROR for a refused address or byte,
// I2C_BUS_ERROR otherwise) and records the port's real answer, which
// scd4x_hal_last_transfer() gives back -- along with whether it was a read,
// since a refused read header after an acknowledged command means something
// else than a refused command (scd4x_source.h, "Busy, not absent").

#ifndef HAC_DRIVERS_SCD4X_SCD4X_HAL_H
#define HAC_DRIVERS_SCD4X_SCD4X_HAL_H

#include <stdbool.h>

#include "hal_status.h"
#include "port_clock.h"
#include "port_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

// Points the HAL at `bus` and `clock` on behalf of `owner`, which only
// identifies the claimant and is never dereferenced. Claiming again with
// the same owner rebinds. HAL_ERR_BUSY if a different owner holds it;
// HAL_ERR_INVALID_ARG if any argument is NULL.
hal_status_t scd4x_hal_claim(const void *owner, port_i2c_t *bus, const port_clock_t *clock);

// Gives the HAL up, if `owner` holds it. Afterwards every HAL call fails
// without touching a bus.
void scd4x_hal_release(const void *owner);

typedef struct {
    hal_status_t status;  // the port's answer; HAL_ERR_INVALID_ARG if unclaimed
    bool was_read;        // a read transfer, not a write
} scd4x_hal_transfer_t;

// The most recent transfer Sensirion's code made through the HAL.
scd4x_hal_transfer_t scd4x_hal_last_transfer(void);

#ifdef __cplusplus
}
#endif

#endif  // HAC_DRIVERS_SCD4X_SCD4X_HAL_H
