// SPDX-License-Identifier: Apache-2.0
//
// A bus scan over port_i2c, and the comparison of what answered against what
// board_config.h says should be there. Phase 2d's deliverable: the scan that
// confirms the wiring before any sensor driver exists.
//
// Pure over the port -- no ESP-IDF, no stdio -- so the host build tests it
// against platform/host/adp_i2c_fake.c (tests/unit/test_i2c_scan.c), compiled
// from here as duration_stats.c is. Printing the report, and knowing which
// addresses to expect, is console.c's job.

#ifndef HAC_APPS_FIRMWARE_ESP32_I2C_SCAN_H
#define HAC_APPS_FIRMWARE_ESP32_I2C_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"
#include "port_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

// The 7-bit addresses a scan probes: everything outside the two reserved
// blocks (0x00-0x07 and 0x78-0x7F, UM10204 table 4).
#define I2C_SCAN_FIRST_ADDR 0x08u
#define I2C_SCAN_LAST_ADDR 0x77u

#define I2C_SCAN_BITMAP_BYTES ((PORT_I2C_ADDR_MAX + 1u) / 8u)

typedef struct {
    uint8_t answered[I2C_SCAN_BITMAP_BYTES];  // one bit per 7-bit address

    // HAL_OK when every address was probed. Otherwise the error that stopped
    // the scan: a probe that neither answered nor was refused means the bus
    // is not working, and probing on would only repeat the same timeout a
    // hundred times.
    hal_status_t status;

    // One past the last address actually probed: I2C_SCAN_LAST_ADDR + 1
    // after a complete scan, the failing address after an incomplete one.
    uint8_t scanned_end;
} i2c_scan_result_t;

// Probes every address from I2C_SCAN_FIRST_ADDR to I2C_SCAN_LAST_ADDR with
// an address-only write (ports/port_i2c.h), each given `probe_timeout_ms`.
void i2c_scan(port_i2c_t *bus, uint32_t probe_timeout_ms, i2c_scan_result_t *out);

bool i2c_scan_answered(const i2c_scan_result_t *r, uint8_t addr);
bool i2c_scan_probed(const i2c_scan_result_t *r, uint8_t addr);
size_t i2c_scan_count(const i2c_scan_result_t *r);

typedef enum {
    I2C_EXPECT_REQUIRED = 0,  // board_config.h says it is there
    I2C_EXPECT_CANDIDATE,     // may be there; absence is not a fault
} i2c_expect_kind_t;

typedef struct {
    uint8_t addr;
    i2c_expect_kind_t kind;
    const char *what;  // borrowed; printed as is
} i2c_expected_t;

typedef enum {
    I2C_ROW_FOUND = 0,    // expected or a candidate, and it answered
    I2C_ROW_MISSING,      // required, and it did not answer
    I2C_ROW_ABSENT,       // a candidate that did not answer
    I2C_ROW_UNEXPECTED,   // answered, but nothing in the table names it
    I2C_ROW_NOT_SCANNED,  // in the table, but the scan never probed it
} i2c_row_status_t;

typedef struct {
    uint8_t addr;
    i2c_row_status_t status;
    const char *what;  // from the table; NULL for an unexpected address
} i2c_scan_row_t;

// One row per address that is in `expected` or answered the scan, in
// ascending address order. An address listed twice in `expected` gets one
// row, described by its first entry. Writes at most `max_rows` rows and
// returns how many it wrote; a row count equal to `max_rows` may mean the
// report was cut short, so size `rows` for every expected address plus the
// 112 a scan can find.
size_t i2c_scan_report(const i2c_scan_result_t *r, const i2c_expected_t *expected,
                       size_t expected_count, i2c_scan_row_t *rows, size_t max_rows);

// "found", "MISSING", "absent", "UNEXPECTED", "not scanned". Upper case is
// what someone reading a pasted log should stop at.
const char *i2c_scan_row_status_name(i2c_row_status_t s);

#ifdef __cplusplus
}
#endif

#endif  // HAC_APPS_FIRMWARE_ESP32_I2C_SCAN_H
