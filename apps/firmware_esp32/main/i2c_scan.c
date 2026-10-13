// SPDX-License-Identifier: Apache-2.0

#include "i2c_scan.h"

#include <string.h>

#define BITS_PER_BYTE 8u

static void mark(i2c_scan_result_t *r, uint8_t addr)
{
    r->answered[addr / BITS_PER_BYTE] |= (uint8_t)(1u << (addr % BITS_PER_BYTE));
}

void i2c_scan(port_i2c_t *bus, uint32_t probe_timeout_ms, i2c_scan_result_t *out)
{
    memset(out, 0, sizeof *out);
    out->status = HAL_OK;
    out->scanned_end = (uint8_t)(I2C_SCAN_LAST_ADDR + 1u);

    for (unsigned addr = I2C_SCAN_FIRST_ADDR; addr <= I2C_SCAN_LAST_ADDR; addr++) {
        const hal_status_t s = bus->write(bus, (uint8_t)addr, NULL, 0u, probe_timeout_ms);
        if (s == HAL_OK) {
            mark(out, (uint8_t)addr);
        } else if (s != HAL_ERR_NOT_FOUND) {
            out->status = s;
            out->scanned_end = (uint8_t)addr;
            return;
        }
    }
}

bool i2c_scan_answered(const i2c_scan_result_t *r, uint8_t addr)
{
    if (addr > PORT_I2C_ADDR_MAX) {
        return false;
    }
    return (r->answered[addr / BITS_PER_BYTE] & (1u << (addr % BITS_PER_BYTE))) != 0u;
}

bool i2c_scan_probed(const i2c_scan_result_t *r, uint8_t addr)
{
    return addr >= I2C_SCAN_FIRST_ADDR && addr < r->scanned_end;
}

size_t i2c_scan_count(const i2c_scan_result_t *r)
{
    size_t n = 0u;
    for (unsigned addr = 0u; addr <= PORT_I2C_ADDR_MAX; addr++) {
        if (i2c_scan_answered(r, (uint8_t)addr)) {
            n++;
        }
    }
    return n;
}

static const i2c_expected_t *find_expected(const i2c_expected_t *expected, size_t count,
                                           uint8_t addr)
{
    for (size_t i = 0; i < count; i++) {
        if (expected[i].addr == addr) {
            return &expected[i];
        }
    }
    return NULL;
}

size_t i2c_scan_report(const i2c_scan_result_t *r, const i2c_expected_t *expected,
                       size_t expected_count, i2c_scan_row_t *rows, size_t max_rows)
{
    size_t n = 0u;
    for (unsigned a = 0u; a <= PORT_I2C_ADDR_MAX && n < max_rows; a++) {
        const uint8_t addr = (uint8_t)a;
        const i2c_expected_t *e = find_expected(expected, expected_count, addr);
        const bool answered = i2c_scan_answered(r, addr);
        if (e == NULL && !answered) {
            continue;
        }

        i2c_scan_row_t *row = &rows[n++];
        row->addr = addr;
        row->what = (e != NULL) ? e->what : NULL;
        if (e == NULL) {
            row->status = I2C_ROW_UNEXPECTED;
        } else if (answered) {
            row->status = I2C_ROW_FOUND;
        } else if (!i2c_scan_probed(r, addr)) {
            row->status = I2C_ROW_NOT_SCANNED;
        } else {
            row->status = (e->kind == I2C_EXPECT_CANDIDATE) ? I2C_ROW_ABSENT : I2C_ROW_MISSING;
        }
    }
    return n;
}

const char *i2c_scan_row_status_name(i2c_row_status_t s)
{
    switch (s) {
        case I2C_ROW_FOUND:
            return "found";
        case I2C_ROW_MISSING:
            return "MISSING";
        case I2C_ROW_ABSENT:
            return "absent";
        case I2C_ROW_UNEXPECTED:
            return "UNEXPECTED";
        case I2C_ROW_NOT_SCANNED:
            return "not scanned";
        default:
            return "?";
    }
}
