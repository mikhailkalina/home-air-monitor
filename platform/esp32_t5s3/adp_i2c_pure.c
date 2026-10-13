// SPDX-License-Identifier: Apache-2.0

#include "adp_i2c_pure.h"

#define US_PER_MS 1000u
#define MS_PER_S 1000u

uint32_t adp_i2c_driver_wait_ms(uint64_t remaining_us, uint32_t tick_hz)
{
    if (remaining_us == 0u) {
        return 0u;
    }

    const uint64_t remaining_ms = (remaining_us + US_PER_MS - 1u) / US_PER_MS;
    const uint64_t ticks = (remaining_ms * tick_hz + MS_PER_S - 1u) / MS_PER_S + 1u;
    const uint64_t wait_ms = (ticks * MS_PER_S + tick_hz - 1u) / tick_hz;

    return wait_ms > ADP_I2C_DRIVER_WAIT_MAX_MS ? ADP_I2C_DRIVER_WAIT_MAX_MS : (uint32_t)wait_ms;
}

hal_status_t adp_i2c_classify_failure(adp_i2c_probe_t probe)
{
    switch (probe) {
        case ADP_I2C_PROBE_NOT_RUN:
            return HAL_ERR_TIMEOUT;
        case ADP_I2C_PROBE_ACK:
            return HAL_ERR_IO;
        case ADP_I2C_PROBE_NACK:
            return HAL_ERR_NOT_FOUND;
        case ADP_I2C_PROBE_TIMEOUT:
            return HAL_ERR_TIMEOUT;
        case ADP_I2C_PROBE_OTHER:
        default:
            return HAL_ERR_IO;
    }
}
