// SPDX-License-Identifier: Apache-2.0

#include "adp_i2c_fake.h"

#include <string.h>

static i2c_model_t *find_model(adp_i2c_fake_t *f, uint8_t addr)
{
    for (size_t i = 0; i < f->slot_count; i++) {
        if (f->slots[i].addr == addr) {
            return f->slots[i].model;
        }
    }
    return NULL;
}

static uint64_t now_ms(const adp_i2c_fake_t *f)
{
    return f->clock != NULL ? f->clock->now_ms : 0u;
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
    i2c_model_t *model = find_model(f, addr);
    if (model == NULL) {
        return HAL_ERR_NOT_FOUND;
    }
    return model->write(model, n > 0u ? d : NULL, n, now_ms(f));
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
    i2c_model_t *model = find_model(f, addr);
    if (model == NULL) {
        return HAL_ERR_NOT_FOUND;
    }
    return model->read(model, d, n, now_ms(f));
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
    i2c_model_t *model = find_model(f, addr);
    if (model == NULL) {
        return HAL_ERR_NOT_FOUND;
    }
    // One transaction: the write, a repeated START, the read.
    const hal_status_t written = model->write(model, w, wn, now_ms(f));
    if (written != HAL_OK) {
        return written;
    }
    return model->read(model, r, rn, now_ms(f));
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

bool adp_i2c_fake_attach(adp_i2c_fake_t *f, uint8_t addr, i2c_model_t *model)
{
    if (addr > PORT_I2C_ADDR_MAX || model == NULL || find_model(f, addr) != NULL ||
        f->slot_count >= ADP_I2C_FAKE_MAX_DEVICES) {
        return false;
    }
    f->slots[f->slot_count].addr = addr;
    f->slots[f->slot_count].model = model;
    f->slot_count++;
    return true;
}

adp_i2c_fake_device_t *adp_i2c_fake_add_device(adp_i2c_fake_t *f, uint8_t addr)
{
    if (f->regfile_count >= ADP_I2C_FAKE_MAX_DEVICES) {
        return NULL;
    }
    regfile_model_t *dev = &f->regfiles[f->regfile_count];
    regfile_model_init(dev);
    if (!adp_i2c_fake_attach(f, addr, regfile_model_port(dev))) {
        return NULL;
    }
    f->regfile_count++;
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
