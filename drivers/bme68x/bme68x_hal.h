// SPDX-License-Identifier: Apache-2.0
//
// The bus and the delay Bosch's BME68x Sensor API (vendor/bme68x.c) asks
// for, over port_i2c and port_clock: the glue that lets Bosch's own code run
// unchanged on the board (over adp_i2c) and on a PC (over adp_i2c_fake and
// its BME680 model).
//
// Unlike Sensirion's HAL (scd4x_hal.h), Bosch's takes a context -- every
// callback gets the bme68x_dev's intf_ptr -- so there are no globals here and
// no claim: two BME68x on one bus, at 0x76 and 0x77, would simply be two
// bme68x_hal_t.
//
// On the wire (BME680 datasheet 6.2): a read is the register address written,
// then a repeated START and the bytes, which the sensor auto-increments --
// one port_i2c write_read(). A write is the register address and its data in
// one transfer; Bosch has already interleaved further (address, data) pairs
// into the data for a multi-register write (bme68x_set_regs()), which is how
// the sensor expects them.
//
// Errors. A callback returns the port's hal_status_t, cast to Bosch's
// int8_t; Bosch turns any non-zero answer into BME68X_E_COM_FAIL and keeps
// the callback's own value in bme68x_dev::intf_rslt, which
// bme68x_hal_status() hands back.
//
// Delays are in microseconds and port_clock's delay_ms() is not:
// bme68x_hal_delay_us() ROUNDS UP to whole milliseconds, because every wait
// Bosch asks for is a minimum. 1 us waits 1 ms, 1001 us waits 2 ms, 0 waits
// nothing. Bosch's own waits are 10 ms multiples (BME68X_PERIOD_RESET,
// BME68X_PERIOD_POLL), so the rounding only ever lengthens a wait this
// project computes itself.

#ifndef HAC_DRIVERS_BME68X_BME68X_HAL_H
#define HAC_DRIVERS_BME68X_BME68X_HAL_H

#include <stdint.h>

#include "bme68x_defs.h"
#include "hal_status.h"
#include "port_clock.h"
#include "port_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    port_i2c_t *bus;
    const port_clock_t *clock;
    uint8_t address;  // BOARD_BME68X_I2C_ADDRESS on the board
} bme68x_hal_t;

// Points `dev`'s read, write and delay_us at `hal`, which must outlive it,
// and selects the I2C interface. Touches no bus.
void bme68x_hal_bind(struct bme68x_dev *dev, bme68x_hal_t *hal);

// The port's answer to the transfer behind the last BME68X_E_COM_FAIL, from
// bme68x_dev::intf_rslt. HAL_ERR_IO if it holds no failure, or a value no
// callback here returns.
hal_status_t bme68x_hal_status(const struct bme68x_dev *dev);

// The callbacks themselves, exposed for their tests. A NULL context, or one
// with no bus or clock, fails (or waits nothing) without touching a bus.
BME68X_INTF_RET_TYPE bme68x_hal_read(uint8_t reg, uint8_t *data, uint32_t len, void *intf_ptr);
BME68X_INTF_RET_TYPE bme68x_hal_write(uint8_t reg, const uint8_t *data, uint32_t len,
                                      void *intf_ptr);
void bme68x_hal_delay_us(uint32_t period_us, void *intf_ptr);

#ifdef __cplusplus
}
#endif

#endif  // HAC_DRIVERS_BME68X_BME68X_HAL_H
