// SPDX-License-Identifier: Apache-2.0
//
// apps/firmware_esp32/main/i2c_scan.c against the host's fake bus, compiled
// from where the firmware compiles it (see tests/CMakeLists.txt).

#include "i2c_scan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "adp_i2c_fake.h"
#include "hac_test.h"

#define PROBE_TIMEOUT_MS 20u

// Arbitrary device addresses on the fake bus -- not the board's.
#define ADDR_A 0x23u
#define ADDR_B 0x51u
#define ADDR_C 0x5Du

#define REPORT_ROWS 128u

static void an_empty_bus_answers_nowhere_and_is_scanned_completely(void)
{
    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, NULL);

    i2c_scan_result_t r;
    i2c_scan(adp_i2c_fake_port(&bus), PROBE_TIMEOUT_MS, &r);

    HAC_CHECK_EQ_INT(r.status, HAL_OK);
    HAC_CHECK_EQ_U64(i2c_scan_count(&r), 0u);
    HAC_CHECK_EQ_U64(r.scanned_end, I2C_SCAN_LAST_ADDR + 1u);
    // Exactly the 112 usable addresses, each probed once.
    HAC_CHECK_EQ_U64(bus.transfers, I2C_SCAN_LAST_ADDR - I2C_SCAN_FIRST_ADDR + 1u);
}

static void the_first_and_last_usable_addresses_are_probed_and_the_reserved_ones_are_not(void)
{
    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, NULL);
    HAC_CHECK(adp_i2c_fake_add_device(&bus, I2C_SCAN_FIRST_ADDR - 1u) != NULL);
    HAC_CHECK(adp_i2c_fake_add_device(&bus, I2C_SCAN_FIRST_ADDR) != NULL);
    HAC_CHECK(adp_i2c_fake_add_device(&bus, I2C_SCAN_LAST_ADDR) != NULL);
    HAC_CHECK(adp_i2c_fake_add_device(&bus, I2C_SCAN_LAST_ADDR + 1u) != NULL);

    i2c_scan_result_t r;
    i2c_scan(adp_i2c_fake_port(&bus), PROBE_TIMEOUT_MS, &r);

    HAC_CHECK(!i2c_scan_answered(&r, I2C_SCAN_FIRST_ADDR - 1u));
    HAC_CHECK(i2c_scan_answered(&r, I2C_SCAN_FIRST_ADDR));
    HAC_CHECK(i2c_scan_answered(&r, I2C_SCAN_LAST_ADDR));
    HAC_CHECK(!i2c_scan_answered(&r, I2C_SCAN_LAST_ADDR + 1u));
    HAC_CHECK_EQ_U64(i2c_scan_count(&r), 2u);

    HAC_CHECK(!i2c_scan_probed(&r, I2C_SCAN_FIRST_ADDR - 1u));
    HAC_CHECK(i2c_scan_probed(&r, I2C_SCAN_LAST_ADDR));
    HAC_CHECK(!i2c_scan_probed(&r, I2C_SCAN_LAST_ADDR + 1u));
}

static void a_probe_neither_answered_nor_refused_stops_the_scan_at_once(void)
{
    adp_clock_virtual_t clock;
    adp_clock_virtual_init(&clock, 1.0);
    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, &clock);
    (void)adp_i2c_fake_add_device(&bus, ADDR_B);
    adp_i2c_fake_hold_scl(&bus, true);

    i2c_scan_result_t r;
    i2c_scan(adp_i2c_fake_port(&bus), PROBE_TIMEOUT_MS, &r);

    HAC_CHECK_EQ_INT(r.status, HAL_ERR_TIMEOUT);
    HAC_CHECK_EQ_U64(r.scanned_end, I2C_SCAN_FIRST_ADDR);
    HAC_CHECK_EQ_U64(bus.transfers, 1u);  // one timeout, not 112 of them
    HAC_CHECK_EQ_U64(clock.now_ms, PROBE_TIMEOUT_MS);
    HAC_CHECK(!i2c_scan_probed(&r, ADDR_B));
}

// A bus that fails one probe part-way through, as a real one can.
typedef struct {
    port_i2c_t port;
    port_i2c_t *inner;
    uint8_t fail_at;
} failing_bus_t;

static hal_status_t failing_write(port_i2c_t *port, uint8_t addr, const uint8_t *d, size_t n,
                                  uint32_t to_ms)
{
    failing_bus_t *f = (failing_bus_t *)port->impl;
    if (addr == f->fail_at) {
        return HAL_ERR_BUSY;
    }
    return f->inner->write(f->inner, addr, d, n, to_ms);
}

static void an_incomplete_scan_reports_where_it_stopped_and_keeps_what_it_found(void)
{
    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, NULL);
    (void)adp_i2c_fake_add_device(&bus, ADDR_A);
    (void)adp_i2c_fake_add_device(&bus, ADDR_C);

    failing_bus_t failing = {.inner = adp_i2c_fake_port(&bus), .fail_at = ADDR_B};
    failing.port.write = failing_write;
    failing.port.impl = &failing;

    i2c_scan_result_t r;
    i2c_scan(&failing.port, PROBE_TIMEOUT_MS, &r);

    HAC_CHECK_EQ_INT(r.status, HAL_ERR_BUSY);
    HAC_CHECK_EQ_U64(r.scanned_end, ADDR_B);
    HAC_CHECK(i2c_scan_answered(&r, ADDR_A));
    HAC_CHECK(!i2c_scan_answered(&r, ADDR_C));  // never reached
    HAC_CHECK(i2c_scan_probed(&r, ADDR_B - 1u));
    HAC_CHECK(!i2c_scan_probed(&r, ADDR_B));
}

static const i2c_scan_row_t *row_for(const i2c_scan_row_t *rows, size_t n, uint8_t addr)
{
    for (size_t i = 0; i < n; i++) {
        if (rows[i].addr == addr) {
            return &rows[i];
        }
    }
    return NULL;
}

static void the_report_names_every_outcome(void)
{
    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, NULL);
    (void)adp_i2c_fake_add_device(&bus, ADDR_A);  // unexpected
    (void)adp_i2c_fake_add_device(&bus, ADDR_B);  // required, found
    (void)adp_i2c_fake_add_device(&bus, ADDR_C);  // candidate, found

    const i2c_expected_t expected[] = {
        {.addr = ADDR_B, .kind = I2C_EXPECT_REQUIRED, .what = "required here"},
        {.addr = 0x5Au, .kind = I2C_EXPECT_REQUIRED, .what = "required, gone"},
        {.addr = ADDR_C, .kind = I2C_EXPECT_CANDIDATE, .what = "candidate here"},
        {.addr = 0x14u, .kind = I2C_EXPECT_CANDIDATE, .what = "candidate, gone"},
        {.addr = 0x03u, .kind = I2C_EXPECT_REQUIRED, .what = "reserved, never probed"},
    };

    i2c_scan_result_t r;
    i2c_scan(adp_i2c_fake_port(&bus), PROBE_TIMEOUT_MS, &r);
    i2c_scan_row_t rows[REPORT_ROWS];
    const size_t n =
        i2c_scan_report(&r, expected, sizeof expected / sizeof expected[0], rows, REPORT_ROWS);

    HAC_CHECK_EQ_U64(n, 6u);
    const i2c_scan_row_t *row;
    HAC_CHECK((row = row_for(rows, n, ADDR_A)) != NULL && row->status == I2C_ROW_UNEXPECTED &&
              row->what == NULL);
    HAC_CHECK((row = row_for(rows, n, ADDR_B)) != NULL && row->status == I2C_ROW_FOUND &&
              strcmp(row->what, "required here") == 0);
    HAC_CHECK((row = row_for(rows, n, 0x5Au)) != NULL && row->status == I2C_ROW_MISSING);
    HAC_CHECK((row = row_for(rows, n, ADDR_C)) != NULL && row->status == I2C_ROW_FOUND);
    HAC_CHECK((row = row_for(rows, n, 0x14u)) != NULL && row->status == I2C_ROW_ABSENT);
    HAC_CHECK((row = row_for(rows, n, 0x03u)) != NULL && row->status == I2C_ROW_NOT_SCANNED);
}

static void report_rows_ascend_and_a_duplicated_entry_appears_once(void)
{
    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, NULL);
    (void)adp_i2c_fake_add_device(&bus, ADDR_C);
    (void)adp_i2c_fake_add_device(&bus, ADDR_A);

    const i2c_expected_t expected[] = {
        {.addr = ADDR_C, .kind = I2C_EXPECT_REQUIRED, .what = "first"},
        {.addr = ADDR_C, .kind = I2C_EXPECT_CANDIDATE, .what = "second"},
        {.addr = ADDR_B, .kind = I2C_EXPECT_REQUIRED, .what = "between"},
    };

    i2c_scan_result_t r;
    i2c_scan(adp_i2c_fake_port(&bus), PROBE_TIMEOUT_MS, &r);
    i2c_scan_row_t rows[REPORT_ROWS];
    const size_t n =
        i2c_scan_report(&r, expected, sizeof expected / sizeof expected[0], rows, REPORT_ROWS);

    HAC_CHECK_EQ_U64(n, 3u);
    HAC_CHECK_EQ_U64(rows[0].addr, ADDR_A);
    HAC_CHECK_EQ_U64(rows[1].addr, ADDR_B);
    HAC_CHECK_EQ_U64(rows[2].addr, ADDR_C);
    HAC_CHECK(strcmp(rows[2].what, "first") == 0);
}

static void an_address_after_an_early_stop_is_not_scanned_rather_than_missing(void)
{
    i2c_scan_result_t r;
    memset(&r, 0, sizeof r);
    r.status = HAL_ERR_TIMEOUT;
    r.scanned_end = ADDR_B;

    const i2c_expected_t expected[] = {
        {.addr = ADDR_A, .kind = I2C_EXPECT_REQUIRED, .what = "before the stop"},
        {.addr = ADDR_B, .kind = I2C_EXPECT_REQUIRED, .what = "at the stop"},
    };
    i2c_scan_row_t rows[REPORT_ROWS];
    const size_t n = i2c_scan_report(&r, expected, 2u, rows, REPORT_ROWS);

    HAC_CHECK_EQ_U64(n, 2u);
    HAC_CHECK_EQ_INT(rows[0].status, I2C_ROW_MISSING);
    HAC_CHECK_EQ_INT(rows[1].status, I2C_ROW_NOT_SCANNED);
}

static void the_report_never_writes_past_max_rows(void)
{
    adp_i2c_fake_t bus;
    adp_i2c_fake_init(&bus, NULL);
    (void)adp_i2c_fake_add_device(&bus, ADDR_A);
    (void)adp_i2c_fake_add_device(&bus, ADDR_B);
    (void)adp_i2c_fake_add_device(&bus, ADDR_C);

    i2c_scan_result_t r;
    i2c_scan(adp_i2c_fake_port(&bus), PROBE_TIMEOUT_MS, &r);

    i2c_scan_row_t rows[3];
    memset(rows, 0xA5, sizeof rows);
    const size_t n = i2c_scan_report(&r, NULL, 0u, rows, 2u);
    HAC_CHECK_EQ_U64(n, 2u);
    HAC_CHECK_EQ_U64(rows[1].addr, ADDR_B);
    HAC_CHECK_EQ_U64(rows[2].addr, 0xA5u);  // untouched
    HAC_CHECK_EQ_U64(i2c_scan_report(&r, NULL, 0u, rows, 0u), 0u);
}

static void every_row_status_has_a_name(void)
{
    HAC_CHECK(strcmp(i2c_scan_row_status_name(I2C_ROW_FOUND), "found") == 0);
    HAC_CHECK(strcmp(i2c_scan_row_status_name(I2C_ROW_MISSING), "MISSING") == 0);
    HAC_CHECK(strcmp(i2c_scan_row_status_name(I2C_ROW_ABSENT), "absent") == 0);
    HAC_CHECK(strcmp(i2c_scan_row_status_name(I2C_ROW_UNEXPECTED), "UNEXPECTED") == 0);
    HAC_CHECK(strcmp(i2c_scan_row_status_name(I2C_ROW_NOT_SCANNED), "not scanned") == 0);
}

int main(void)
{
    HAC_RUN(an_empty_bus_answers_nowhere_and_is_scanned_completely);
    HAC_RUN(the_first_and_last_usable_addresses_are_probed_and_the_reserved_ones_are_not);
    HAC_RUN(a_probe_neither_answered_nor_refused_stops_the_scan_at_once);
    HAC_RUN(an_incomplete_scan_reports_where_it_stopped_and_keeps_what_it_found);
    HAC_RUN(the_report_names_every_outcome);
    HAC_RUN(report_rows_ascend_and_a_duplicated_entry_appears_once);
    HAC_RUN(an_address_after_an_early_stop_is_not_scanned_rather_than_missing);
    HAC_RUN(the_report_never_writes_past_max_rows);
    HAC_RUN(every_row_status_has_a_name);
    return hac_test_summary();
}
