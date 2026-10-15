// SPDX-License-Identifier: Apache-2.0
//
// A device on the host's fake I2C bus (platform/host/adp_i2c_fake.c), as the
// bus sees it: something that acknowledges its address or does not, takes
// bytes, and gives bytes back.
//
// The bus owns addressing, the argument checks of ports/port_i2c.h and the
// bus faults (a hang, a stall). A model owns only what the device itself
// does, which is where devices differ: a register file
// (regfile_model.h) stores what is written at a pointer; an SCD41
// (scd41_model.h) interprets 16-bit commands, checks CRCs and refuses its
// address while it is busy.
//
// The bus hands every call the virtual clock's current time, so a model can
// keep its own timing -- an execution window, a measurement interval --
// without holding a clock of its own. On a bus built without a clock, now_ms
// is always 0.

#ifndef HAC_PLATFORM_HOST_I2C_MODELS_I2C_MODEL_H
#define HAC_PLATFORM_HOST_I2C_MODELS_I2C_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct i2c_model_s i2c_model_t;

struct i2c_model_s {
    // START (or a repeated START), address+W, the n bytes of `d`. n == 0 is
    // the address-only probe, and `d` is then NULL.
    //
    //   HAL_OK             the address and every byte were acknowledged
    //   HAL_ERR_NOT_FOUND  the address was refused; nothing reached the device
    //   HAL_ERR_IO         the address was acknowledged, a data byte refused
    hal_status_t (*write)(i2c_model_t *self, const uint8_t *d, size_t n, uint64_t now_ms);

    // START (or a repeated START), address+R, n >= 1 bytes into `d`. It is the
    // master that acknowledges each byte of a read, so once the device has
    // acknowledged its address the transfer runs to the end; a device with
    // nothing more to say leaves SDA released, and the master reads 0xFF.
    //
    //   HAL_OK             the address was acknowledged and `d` filled
    //   HAL_ERR_NOT_FOUND  the address was refused; `d` is untouched
    hal_status_t (*read)(i2c_model_t *self, uint8_t *d, size_t n, uint64_t now_ms);

    void *impl;
};

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_I2C_MODELS_I2C_MODEL_H
