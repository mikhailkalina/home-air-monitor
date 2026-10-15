// SPDX-License-Identifier: Apache-2.0
//
// port_i2c on the host: a bus with device models on it (docs/architecture.md
// 5.4, the "fake-i2c" substitution point). Drivers in drivers/ run against
// this on a PC exactly as they run against platform/esp32_t5s3/adp_i2c.c on
// the board, and tests/contract/port_i2c_contract.c holds the two to the same
// rules.
//
// The bus does what a bus does -- checks the arguments ports/port_i2c.h
// requires, routes each transfer to whatever is attached at its address, and
// reports an address nothing is attached to as HAL_ERR_NOT_FOUND. What a
// device does with the bytes is its model's business (i2c_models/i2c_model.h):
//
//   * adp_i2c_fake_add_device() attaches a register file
//     (i2c_models/regfile_model.h), the commonest protocol and the one the
//     contract and the bus scan need;
//   * adp_i2c_fake_attach() attaches any other model, such as the SCD41's
//     command interpreter (i2c_models/scd41_model.h).
//
// Every model call gets the virtual clock's time, so models keep time without
// a clock of their own.
//
// Two bus faults can be injected, the same two the contract needs on the
// board:
//
//   * a hang -- a slave holding SDA low mid-transfer. Every transfer fails
//     with HAL_ERR_TIMEOUT (on real hardware: arbitration lost at the first
//     address bit) until recover() clocks the slave free.
//   * a stall -- SCL held low. Every transfer advances the clock by its
//     whole deadline, as a real timeout would, and fails with
//     HAL_ERR_TIMEOUT. recover() cannot clock through a held SCL either.

#ifndef HAC_PLATFORM_HOST_ADP_I2C_FAKE_H
#define HAC_PLATFORM_HOST_ADP_I2C_FAKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "adp_clock_virtual.h"
#include "i2c_models/i2c_model.h"
#include "i2c_models/regfile_model.h"
#include "port_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

// Enough for every device the board carries plus phase 3's sensors, twice.
#define ADP_I2C_FAKE_MAX_DEVICES 8u

// The bus-clear procedure's limit (UM10204 3.1.16): a slave that has not let
// go of SDA after nine clocks is not going to.
#define ADP_I2C_FAKE_RECOVERY_MAX_PULSES 9u

// What adp_i2c_fake_add_device() attaches.
typedef regfile_model_t adp_i2c_fake_device_t;

typedef struct {
    uint8_t addr;
    i2c_model_t *model;  // borrowed
} adp_i2c_fake_slot_t;

typedef struct {
    port_i2c_t port;

    adp_i2c_fake_slot_t slots[ADP_I2C_FAKE_MAX_DEVICES];
    size_t slot_count;

    // Storage for the register files adp_i2c_fake_add_device() attaches.
    regfile_model_t regfiles[ADP_I2C_FAKE_MAX_DEVICES];
    size_t regfile_count;

    // Gives the models their time, and is advanced by a stalled transfer's
    // deadline. May be NULL: models then always see 0, and a stall fails
    // without time passing.
    adp_clock_virtual_t *clock;

    // Injected faults.
    bool sda_stuck;
    unsigned pulses_to_release;  // clocks the stuck slave needs before it lets go
    bool scl_held;

    // Observability: what the code under test asked of the bus.
    uint32_t transfers;  // every write/read/write_read that reached the bus
    uint32_t recoveries;
    unsigned last_recover_pulses;
} adp_i2c_fake_t;

// An empty, healthy bus. `clock` is borrowed and may be NULL.
void adp_i2c_fake_init(adp_i2c_fake_t *f, adp_clock_virtual_t *clock);

port_i2c_t *adp_i2c_fake_port(adp_i2c_fake_t *f);

// Attaches a register-file device, all registers zero, pointer at 0. Returns
// NULL if `addr` is not a 7-bit address, already taken, or the bus is full.
adp_i2c_fake_device_t *adp_i2c_fake_add_device(adp_i2c_fake_t *f, uint8_t addr);

// Attaches `model` at `addr`. The model is borrowed and must outlive the bus.
// Returns false, attaching nothing, if `addr` is not a 7-bit address, already
// taken, or the bus is full.
bool adp_i2c_fake_attach(adp_i2c_fake_t *f, uint8_t addr, i2c_model_t *model);

// Leaves a slave holding SDA low. It lets go after `pulses_to_release` SCL
// clocks of a recover() -- 1 to 9 is a slave recover() can free; anything
// above ADP_I2C_FAKE_RECOVERY_MAX_PULSES models one it cannot.
void adp_i2c_fake_hang(adp_i2c_fake_t *f, unsigned pulses_to_release);

// Holds SCL low (true) or lets it go (false).
void adp_i2c_fake_hold_scl(adp_i2c_fake_t *f, bool held);

bool adp_i2c_fake_sda_held_low(const adp_i2c_fake_t *f);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_ADP_I2C_FAKE_H
