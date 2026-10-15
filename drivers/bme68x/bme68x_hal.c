// SPDX-License-Identifier: Apache-2.0

#include "bme68x_hal.h"

#include <stddef.h>
#include <string.h>

#include "bme68x_limits.h"

#define US_PER_MS 1000u

// Bosch's widest write: bme68x_set_regs() interleaves up to
// BME68X_LEN_INTERLEAVE_BUFF / 2 (address, data) pairs and hands the first
// address as `reg`, the other 2n - 1 bytes as data -- 20 bytes on the wire.
#define WRITE_MAX_BYTES BME68X_LEN_INTERLEAVE_BUFF

// The callbacks carry a hal_status_t in Bosch's int8_t.
_Static_assert(HAL_ERR_NOT_READY <= INT8_MAX, "hal_status_t must fit BME68X_INTF_RET_TYPE");

static BME68X_INTF_RET_TYPE answer(hal_status_t status)
{
    return (BME68X_INTF_RET_TYPE)status;
}

BME68X_INTF_RET_TYPE bme68x_hal_read(uint8_t reg, uint8_t *data, uint32_t len, void *intf_ptr)
{
    bme68x_hal_t *hal = (bme68x_hal_t *)intf_ptr;
    if (hal == NULL || hal->bus == NULL) {
        return answer(HAL_ERR_INVALID_ARG);
    }
    return answer(hal->bus->write_read(hal->bus, hal->address, &reg, 1u, data, len,
                                       HAC_BME68X_TRANSFER_TIMEOUT_MS));
}

BME68X_INTF_RET_TYPE bme68x_hal_write(uint8_t reg, const uint8_t *data, uint32_t len,
                                      void *intf_ptr)
{
    bme68x_hal_t *hal = (bme68x_hal_t *)intf_ptr;
    if (hal == NULL || hal->bus == NULL || (data == NULL && len > 0u) ||
        len > WRITE_MAX_BYTES - 1u) {
        return answer(HAL_ERR_INVALID_ARG);
    }
    uint8_t buffer[WRITE_MAX_BYTES];
    buffer[0] = reg;
    if (len > 0u) {
        memcpy(&buffer[1], data, len);
    }
    return answer(hal->bus->write(hal->bus, hal->address, buffer, (size_t)len + 1u,
                                  HAC_BME68X_TRANSFER_TIMEOUT_MS));
}

void bme68x_hal_delay_us(uint32_t period_us, void *intf_ptr)
{
    const bme68x_hal_t *hal = (const bme68x_hal_t *)intf_ptr;
    // Rounded up: every wait Bosch asks for is a minimum (bme68x_hal.h).
    const uint32_t ms = period_us / US_PER_MS + ((period_us % US_PER_MS) != 0u ? 1u : 0u);
    if (hal != NULL && hal->clock != NULL && ms > 0u) {
        hal->clock->delay_ms(hal->clock, ms);
    }
}

void bme68x_hal_bind(struct bme68x_dev *dev, bme68x_hal_t *hal)
{
    dev->intf = BME68X_I2C_INTF;
    dev->intf_ptr = hal;
    dev->read = bme68x_hal_read;
    dev->write = bme68x_hal_write;
    dev->delay_us = bme68x_hal_delay_us;
}

hal_status_t bme68x_hal_status(const struct bme68x_dev *dev)
{
    const int rslt = dev->intf_rslt;
    if (rslt <= (int)HAL_OK || rslt > (int)HAL_ERR_NOT_READY) {
        return HAL_ERR_IO;
    }
    return (hal_status_t)rslt;
}
