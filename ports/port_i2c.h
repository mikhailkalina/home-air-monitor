// SPDX-License-Identifier: Apache-2.0
//
// The I2C bus port (docs/architecture.md 5.4): the lower of the two
// substitution points for sensor data. Drivers in drivers/ talk to this, and
// nothing else, so they build and run on a PC against a register-level model
// (platform/host/adp_i2c_fake.c) as well as on the board
// (platform/esp32_t5s3/adp_i2c.c).
//
// Every rule below is exercised against every implementation by
// tests/contract/port_i2c_contract.c, on the host and on the device. Where an
// implementation's underlying driver behaves differently, the adapter
// reconciles it; the caller never sees the difference.
//
// Addresses are 7-bit (0x00..0x7F), without the R/W bit. Anything larger is
// HAL_ERR_INVALID_ARG and puts nothing on the bus.
//
// Zero-length transfers:
//
//   * write() with n == 0 is an address-only transaction -- START, address
//     with the write bit, STOP. It answers "does anything acknowledge this
//     address?" and is what a bus scan is built from: HAL_OK if the address
//     was acknowledged, HAL_ERR_NOT_FOUND if not. `d` may be NULL.
//   * read() with n == 0 is HAL_ERR_INVALID_ARG. It cannot exist on the wire:
//     once a slave has acknowledged a read address it owns SDA and is already
//     shifting out its first byte, and the only way to take the line back is
//     to clock that byte in and refuse it.
//   * write_read() with wn == 0 or rn == 0 is HAL_ERR_INVALID_ARG; use read()
//     or write().
//
// A NULL buffer with a non-zero length is HAL_ERR_INVALID_ARG.
//
// Timeouts: `to_ms` bounds the whole call, including waiting for another
// caller to finish with the bus. It is at least 1; 0 is HAL_ERR_INVALID_ARG.
// An implementation must honour it in both directions:
//
//   * never give up on a transfer before `to_ms` has passed for lack of time
//     -- a deadline shorter than the platform's scheduler tick still lets a
//     healthy transfer complete (see adp_i2c.c for the driver behaviour that
//     makes this a real requirement rather than a truism);
//   * return no later than `to_ms` plus the platform's scheduling granularity.
//     A bus that cannot complete -- a slave stretching SCL forever, a line
//     held low -- costs the caller at most that, never a hang.
//
// It may return HAL_ERR_TIMEOUT earlier than `to_ms` when the bus hardware
// itself gives up first, e.g. on a clock-stretch limit.
//
// Errors, in what they tell the caller:
//
//   HAL_ERR_NOT_FOUND   nothing acknowledged the address: absent, unpowered,
//                       or a device that refuses its address while busy
//                       (several Sensirion parts do).
//   HAL_ERR_IO          the address was acknowledged but the transfer still
//                       failed, e.g. a data byte was refused.
//   HAL_ERR_TIMEOUT     the transfer could not complete: SCL held low past
//                       the deadline or the hardware's stretch limit, a line
//                       stuck, arbitration lost.
//
// A line held low by a stuck slave reaches the caller as HAL_ERR_TIMEOUT or
// HAL_ERR_IO, depending on where in the transfer the controller notices --
// never as HAL_ERR_NOT_FOUND, which would blame the device for the bus. After
// either, recover() is the remedy to try.
//   HAL_ERR_BUSY        another caller held the bus past the deadline.
//   HAL_ERR_INVALID_ARG see above; nothing was put on the bus.
//
// After a read from an address nothing answers, the result is
// HAL_ERR_NOT_FOUND or HAL_ERR_IO -- some controllers cannot tell an address
// NACK from a data NACK -- and never a hang.

#ifndef HAC_PORTS_PORT_I2C_H
#define HAC_PORTS_PORT_I2C_H

#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"

#ifdef __cplusplus
extern "C" {
#endif

// The highest 7-bit address. Addresses are not range-checked against the
// reserved blocks (0x00-0x07, 0x78-0x7F): general call is a legitimate write.
#define PORT_I2C_ADDR_MAX 0x7Fu

typedef struct port_i2c_s port_i2c_t;

struct port_i2c_s {
    // START, address+W, the n bytes of `d`, STOP. n == 0 is the
    // address-only probe described above.
    hal_status_t (*write)(port_i2c_t *self, uint8_t addr, const uint8_t *d, size_t n,
                          uint32_t to_ms);

    // START, address+R, n bytes into `d` (the last one NACKed), STOP. n >= 1.
    hal_status_t (*read)(port_i2c_t *self, uint8_t addr, uint8_t *d, size_t n, uint32_t to_ms);

    // START, address+W, the wn bytes of `w`, repeated START, address+R, rn
    // bytes into `r`, STOP -- one transaction, so no other master or caller
    // can slip in between the register pointer and the read. wn, rn >= 1.
    hal_status_t (*write_read)(port_i2c_t *self, uint8_t addr, const uint8_t *w, size_t wn,
                               uint8_t *r, size_t rn, uint32_t to_ms);

    // The I2C bus-clear procedure (UM10204 3.1.16): clock SCL until a slave
    // stuck mid-byte releases SDA -- at most nine pulses -- then issue a STOP,
    // then bring the controller back to a known state. Safe on a healthy bus.
    //
    // HAL_OK when both lines read high afterwards and the bus is usable;
    // HAL_ERR_IO when a line is still held low (a slave nine clocks could not
    // free, or a short) -- the bus is re-initialised either way, so later
    // calls fail fast rather than misbehave.
    hal_status_t (*recover)(port_i2c_t *self);

    void *impl;
};

#ifdef __cplusplus
}
#endif

#endif  // HAC_PORTS_PORT_I2C_H
