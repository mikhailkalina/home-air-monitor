// SPDX-License-Identifier: Apache-2.0

#include "regfile_model.h"

#include <string.h>

static hal_status_t regfile_write(i2c_model_t *self, const uint8_t *d, size_t n, uint64_t now_ms)
{
    (void)now_ms;
    regfile_model_t *m = (regfile_model_t *)self->impl;
    if (n == 0u) {
        return HAL_OK;  // a probe: the address is acknowledged, nothing changes
    }
    m->pointer = d[0];
    for (size_t i = 1; i < n; i++) {
        m->regs[m->pointer] = d[i];
        m->pointer = (uint8_t)(m->pointer + 1u);
    }
    return HAL_OK;
}

static hal_status_t regfile_read(i2c_model_t *self, uint8_t *d, size_t n, uint64_t now_ms)
{
    (void)now_ms;
    regfile_model_t *m = (regfile_model_t *)self->impl;
    for (size_t i = 0; i < n; i++) {
        d[i] = m->regs[m->pointer];
        m->pointer = (uint8_t)(m->pointer + 1u);
    }
    return HAL_OK;
}

void regfile_model_init(regfile_model_t *m)
{
    memset(m, 0, sizeof *m);
    m->model.write = regfile_write;
    m->model.read = regfile_read;
    m->model.impl = m;
}

i2c_model_t *regfile_model_port(regfile_model_t *m)
{
    return &m->model;
}
