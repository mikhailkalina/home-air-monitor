// SPDX-License-Identifier: Apache-2.0
//
// The commonest I2C device protocol, as an i2c_model_t: a register file. The
// PCF8563, the BME68x and most sensors work this way. The first byte of a
// write sets the register pointer and every further byte is stored at the
// pointer; a read returns bytes from the pointer. The pointer advances after
// each byte and wraps at 256.
//
// This was the whole of phase 2d's fake bus; it is now one model among
// several, and still the one adp_i2c_fake_add_device() attaches, so the
// port_i2c contract and the bus-scan tests see the same device as before.

#ifndef HAC_PLATFORM_HOST_I2C_MODELS_REGFILE_MODEL_H
#define HAC_PLATFORM_HOST_I2C_MODELS_REGFILE_MODEL_H

#include <stdint.h>

#include "i2c_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REGFILE_MODEL_REGISTERS 256u

typedef struct {
    i2c_model_t model;
    uint8_t regs[REGFILE_MODEL_REGISTERS];
    uint8_t pointer;
} regfile_model_t;

// Every register zero, the pointer at 0.
void regfile_model_init(regfile_model_t *m);

i2c_model_t *regfile_model_port(regfile_model_t *m);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_I2C_MODELS_REGFILE_MODEL_H
