// SPDX-License-Identifier: Apache-2.0

#include "adp_i2c_fake.h"

#include <string.h>

static adp_i2c_fake_device_t *find_device(adp_i2c_fake_t *f, uint8_t addr)
{
    for (size_t i = 0; i < f->device_count; i++) {
        if (f->devices[i].addr == addr) {
            return &f->devices[i];
        }
    }
    return NULL;
}

// The checks ports/port_i2c.h requires before anything reaches the bus.
// Shared by all three transfer calls so their rejections cannot drift apart.
static bool args_valid(uint8_t addr, uint32_t to_ms)
{
    return addr <= PORT_I2C_ADDR_MAX && to_ms > 0u;
}

// What the bus itself does to a transfer before any device sees it. Returns
// HAL_OK when the transfer may proceed.
static hal_status_t bus_gate(adp_i2c_fake_t *f, uint32_t to_ms)
{
    f->transfers++;
    if (f->scl_held) {
        // The controller waits for SCL until the caller's deadline.
        if (f->clock != NULL) {
            adp_clock_virtual_advance(f->clock, to_ms);
        }
        return HAL_ERR_TIMEOUT;
    }
    if (f->sda_stuck) {
        // A master cannot even send its first address bit: it releases SDA
        // for a 1 and reads back 0, and loses arbitration at once.
        return HAL_ERR_TIMEOUT;
    }
    return HAL_OK;
}

static void device_write(adp_i2c_fake_device_t *dev, const uint8_t *d, size_t n)
{
    if (n == 0u) {
        return;
    }
    dev->pointer = d[0];
    for (size_t i = 1; i < n; i++) {
        dev->regs[dev->pointer] = d[i];
        dev->pointer = (uint8_t)(dev->pointer + 1u);
    }
}

static void device_read(adp_i2c_fake_device_t *dev, uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        d[i] = dev->regs[dev->pointer];
        dev->pointer = (uint8_t)(dev->pointer + 1u);
    }
}

static hal_status_t fake_write(port_i2c_t *port, uint8_t addr, const uint8_t *d, size_t n,
                               uint32_t to_ms)
{
    adp_i2c_fake_t *f = (adp_i2c_fake_t *)port->impl;
    if (!args_valid(addr, to_ms) || (d == NULL && n > 0u)) {
        return HAL_ERR_INVALID_ARG;
    }
    const hal_status_t gate = bus_gate(f, to_ms);
    if (gate != HAL_OK) {
        return gate;
    }
    adp_i2c_fake_device_t *dev = find_device(f, addr);
    if (dev == NULL) {
        return HAL_ERR_NOT_FOUND;
    }
    device_write(dev, d, n);
    return HAL_OK;
}

static hal_status_t fake_read(port_i2c_t *port, uint8_t addr, uint8_t *d, size_t n, uint32_t to_ms)
{
    adp_i2c_fake_t *f = (adp_i2c_fake_t *)port->impl;
    if (!args_valid(addr, to_ms) || d == NULL || n == 0u) {
        return HAL_ERR_INVALID_ARG;
    }
    const hal_status_t gate = bus_gate(f, to_ms);
    if (gate != HAL_OK) {
        return gate;
    }
    adp_i2c_fake_device_t *dev = find_device(f, addr);
    if (dev == NULL) {
        return HAL_ERR_NOT_FOUND;
    }
    device_read(dev, d, n);
    return HAL_OK;
}

static hal_status_t fake_write_read(port_i2c_t *port, uint8_t addr, const uint8_t *w, size_t wn,
                                    uint8_t *r, size_t rn, uint32_t to_ms)
{
    adp_i2c_fake_t *f = (adp_i2c_fake_t *)port->impl;
    if (!args_valid(addr, to_ms) || w == NULL || wn == 0u || r == NULL || rn == 0u) {
        return HAL_ERR_INVALID_ARG;
    }
    const hal_status_t gate = bus_gate(f, to_ms);
    if (gate != HAL_OK) {
        return gate;
    }
    adp_i2c_fake_device_t *dev = find_device(f, addr);
    if (dev == NULL) {
        return HAL_ERR_NOT_FOUND;
    }
    device_write(dev, w, wn);
    device_read(dev, r, rn);
    return HAL_OK;
}

static hal_status_t fake_recover(port_i2c_t *port)
{
    adp_i2c_fake_t *f = (adp_i2c_fake_t *)port->impl;
    f->recoveries++;
    f->last_recover_pulses = 0u;

    if (f->scl_held) {
        // Nothing to clock with: SCL does not rise however often it is
        // released.
        return HAL_ERR_IO;
    }
    if (f->sda_stuck) {
        if (f->pulses_to_release > ADP_I2C_FAKE_RECOVERY_MAX_PULSES) {
            f->last_recover_pulses = ADP_I2C_FAKE_RECOVERY_MAX_PULSES;
            return HAL_ERR_IO;
        }
        f->last_recover_pulses = f->pulses_to_release;
        f->sda_stuck = false;
    }
    return HAL_OK;
}

void adp_i2c_fake_init(adp_i2c_fake_t *f, adp_clock_virtual_t *clock)
{
    memset(f, 0, sizeof *f);
    f->clock = clock;
    f->port.write = fake_write;
    f->port.read = fake_read;
    f->port.write_read = fake_write_read;
    f->port.recover = fake_recover;
    f->port.impl = f;
}

port_i2c_t *adp_i2c_fake_port(adp_i2c_fake_t *f)
{
    return &f->port;
}

adp_i2c_fake_device_t *adp_i2c_fake_add_device(adp_i2c_fake_t *f, uint8_t addr)
{
    if (addr > PORT_I2C_ADDR_MAX || find_device(f, addr) != NULL ||
        f->device_count >= ADP_I2C_FAKE_MAX_DEVICES) {
        return NULL;
    }
    adp_i2c_fake_device_t *dev = &f->devices[f->device_count++];
    memset(dev, 0, sizeof *dev);
    dev->addr = addr;
    return dev;
}

void adp_i2c_fake_hang(adp_i2c_fake_t *f, unsigned pulses_to_release)
{
    f->sda_stuck = true;
    f->pulses_to_release = pulses_to_release;
}

void adp_i2c_fake_hold_scl(adp_i2c_fake_t *f, bool held)
{
    f->scl_held = held;
}

bool adp_i2c_fake_sda_held_low(const adp_i2c_fake_t *f)
{
    return f->sda_stuck;
}
