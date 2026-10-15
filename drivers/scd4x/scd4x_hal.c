// SPDX-License-Identifier: Apache-2.0
//
// Implements vendor/sensirion_i2c_hal.h. Sensirion ships a template for this
// file (sensirion_i2c_hal.c, every function a TODO); it is not vendored,
// this is what replaces it.

#include "scd4x_hal.h"

#include <stddef.h>
#include <stdint.h>

#include "scd4x_limits.h"
#include "sensirion_common.h"
#include "sensirion_i2c.h"
#include "sensirion_i2c_hal.h"

#define US_PER_MS 1000u

static const void *s_owner;
static port_i2c_t *s_bus;
static const port_clock_t *s_clock;
static scd4x_hal_transfer_t s_last = {.status = HAL_ERR_INVALID_ARG, .was_read = false};

hal_status_t scd4x_hal_claim(const void *owner, port_i2c_t *bus, const port_clock_t *clock)
{
    if (owner == NULL || bus == NULL || clock == NULL) {
        return HAL_ERR_INVALID_ARG;
    }
    if (s_owner != NULL && s_owner != owner) {
        return HAL_ERR_BUSY;
    }
    s_owner = owner;
    s_bus = bus;
    s_clock = clock;
    return HAL_OK;
}

void scd4x_hal_release(const void *owner)
{
    if (owner != NULL && owner == s_owner) {
        s_owner = NULL;
        s_bus = NULL;
        s_clock = NULL;
    }
}

scd4x_hal_transfer_t scd4x_hal_last_transfer(void)
{
    return s_last;
}

static int8_t record(hal_status_t status, bool was_read)
{
    s_last.status = status;
    s_last.was_read = was_read;
    switch (status) {
        case HAL_OK:
            return NO_ERROR;
        case HAL_ERR_NOT_FOUND:
        case HAL_ERR_IO:
            return I2C_NACK_ERROR;
        default:
            return I2C_BUS_ERROR;
    }
}

int16_t sensirion_i2c_hal_select_bus(uint8_t bus_idx)
{
    // One bus: the board has one, and the claim names it.
    return bus_idx == 0u ? NO_ERROR : NOT_IMPLEMENTED_ERROR;
}

void sensirion_i2c_hal_init(void)
{
    // The bus belongs to the composition root, which brings it up before
    // any driver claims it.
}

void sensirion_i2c_hal_free(void)
{
}

int8_t sensirion_i2c_hal_read(uint8_t address, uint8_t *data, uint8_t count)
{
    if (s_bus == NULL) {
        return record(HAL_ERR_INVALID_ARG, true);
    }
    return record(s_bus->read(s_bus, address, data, count, SCD4X_TRANSFER_TIMEOUT_MS), true);
}

int8_t sensirion_i2c_hal_write(uint8_t address, const uint8_t *data, uint8_t count)
{
    if (s_bus == NULL) {
        return record(HAL_ERR_INVALID_ARG, false);
    }
    return record(s_bus->write(s_bus, address, data, count, SCD4X_TRANSFER_TIMEOUT_MS), false);
}

void sensirion_i2c_hal_sleep_usec(uint32_t useconds)
{
    // Rounded up: the HAL's contract is "no less than", and every execution
    // time in the datasheet is a maximum.
    const uint32_t ms = useconds / US_PER_MS + ((useconds % US_PER_MS) != 0u ? 1u : 0u);
    if (s_clock != NULL && ms > 0u) {
        s_clock->delay_ms(s_clock, ms);
    }
}
