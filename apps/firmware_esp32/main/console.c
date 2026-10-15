// SPDX-License-Identifier: Apache-2.0

#include "console.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "esp_console.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "board_config.h"
#include "i2c_scan.h"

// No trailing space: esp_console_setup_prompt() appends one itself.
#define CONSOLE_PROMPT "hac>"

// How long `restart` lets its last line drain to USB-Serial-JTAG before the
// reset cuts the port off. A line takes well under a millisecond at USB full
// speed; usleep() sleeps whole 10 ms ticks, so this is five of them.
#define RESTART_DRAIN_US 50000u

// What board_config.h says is on the bus, each entry printed with the name of
// the constant it came from so a surprise in the scan points straight at the
// line to correct. The GT911's second address is a candidate: it latches one
// of the two at power-up, and only 0x5D has been seen (board_config.h,
// "touch panel"). So is the BME68x's: SDO picks one of two, and which one is
// not yet seen (board_config.h, "sensors"). The sensors are required, so a
// board without them wired says MISSING -- which is the point of the scan.
//
// EXPECT() expands to the fields only; the braces stay at the use site. A
// braced initializer as a macro body is laid out differently by clang-format
// 18 (CI's, Ubuntu 24.04) and 21, with no layout both accept.
#define EXPECT(addr_constant, expect_kind, description) \
    .addr = (addr_constant), .kind = (expect_kind), .what = description " (" #addr_constant ")"

static const i2c_expected_t EXPECTED[] = {
    {EXPECT(BOARD_RTC_I2C_ADDRESS, I2C_EXPECT_REQUIRED, "rtc: PCF8563")},
    {EXPECT(BOARD_TOUCH_I2C_ADDRESS, I2C_EXPECT_REQUIRED, "touch: GT911, INT low at reset")},
    {EXPECT(BOARD_TOUCH_ALT_I2C_ADDRESS, I2C_EXPECT_CANDIDATE,
            "touch: GT911, if INT was high at reset")},
    {EXPECT(BOARD_SCD4X_I2C_ADDRESS, I2C_EXPECT_REQUIRED, "co2: SCD4x, its only address")},
    {EXPECT(BOARD_BME68X_I2C_ADDRESS, I2C_EXPECT_REQUIRED, "env: BME68x, SDO high")},
    {EXPECT(BOARD_BME68X_ALT_I2C_ADDRESS, I2C_EXPECT_CANDIDATE, "env: BME68x, if SDO is low")},
};
#define EXPECTED_COUNT (sizeof EXPECTED / sizeof EXPECTED[0])

// Every expected address plus every address a scan can find. Static rather
// than on the stack: ~1.4 KB is a third of the console task's stack, and the
// boot-time scan and the console never run at the same time.
#define REPORT_ROWS (EXPECTED_COUNT + (I2C_SCAN_LAST_ADDR - I2C_SCAN_FIRST_ADDR + 1u))
static i2c_scan_row_t s_rows[REPORT_ROWS];

static const char *hal_status_text(hal_status_t s)
{
    switch (s) {
        case HAL_OK:
            return "ok";
        case HAL_ERR_TIMEOUT:
            return "timeout";
        case HAL_ERR_IO:
            return "io";
        case HAL_ERR_NOT_FOUND:
            return "not_found";
        case HAL_ERR_INVALID_ARG:
            return "invalid_arg";
        case HAL_ERR_NO_MEM:
            return "no_mem";
        case HAL_ERR_UNSUPPORTED:
            return "unsupported";
        case HAL_ERR_BUSY:
            return "busy";
        case HAL_ERR_NOT_READY:
            return "not_ready";
        default:
            return "?";
    }
}

static const char *level_text(bool high)
{
    return high ? "high" : "LOW";
}

void console_print_i2c_scan(adp_i2c_t *bus)
{
    bool sda_high = false;
    bool scl_high = false;
    adp_i2c_lines(bus, &sda_high, &scl_high);
    printf("I2C_SCAN bus sda_gpio=%d scl_gpio=%d scl_hz=%u, idle lines sda=%s scl=%s\n",
           BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO, (unsigned)BOARD_I2C_SCL_HZ, level_text(sda_high),
           level_text(scl_high));

    // Timed because the first boot with the scan showed about a second between
    // "bus up" and the console starting -- ~9 ms per address if it is all the
    // scan, which nothing in the driver's probe path explains (ADR 0011).
    i2c_scan_result_t result;
    const int64_t start_us = esp_timer_get_time();
    i2c_scan(adp_i2c_port(bus), BOARD_I2C_PROBE_TIMEOUT_MS, &result);
    const int64_t scan_us = esp_timer_get_time() - start_us;
    const size_t n = i2c_scan_report(&result, EXPECTED, EXPECTED_COUNT, s_rows, REPORT_ROWS);

    unsigned missing = 0u;
    unsigned unexpected = 0u;
    for (size_t i = 0; i < n; i++) {
        const i2c_scan_row_t *row = &s_rows[i];
        printf("I2C_SCAN 0x%02X %-11s %s\n", row->addr, i2c_scan_row_status_name(row->status),
               row->what != NULL ? row->what : "not in board_config.h");
        missing += (row->status == I2C_ROW_MISSING) ? 1u : 0u;
        unexpected += (row->status == I2C_ROW_UNEXPECTED) ? 1u : 0u;
    }

    if (result.status != HAL_OK) {
        printf("I2C_SCAN stopped at 0x%02X: %s -- addresses from there on were not probed; "
               "try i2c_recover\n",
               result.scanned_end, hal_status_text(result.status));
    }
    printf("I2C_SCAN_RESULT answered=%u missing=%u unexpected=%u scan=%s scan_ms=%lld\n",
           (unsigned)i2c_scan_count(&result), missing, unexpected,
           result.status == HAL_OK ? "complete" : "INCOMPLETE", (long long)(scan_us / 1000));
}

static int cmd_i2c_scan(void *context, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    console_print_i2c_scan((adp_i2c_t *)context);
    return 0;
}

static int cmd_i2c_recover(void *context, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    adp_i2c_t *bus = (adp_i2c_t *)context;
    port_i2c_t *port = adp_i2c_port(bus);

    const hal_status_t status = port->recover(port);
    bool sda_high = false;
    bool scl_high = false;
    adp_i2c_lines(bus, &sda_high, &scl_high);
    printf("I2C_RECOVER status=%s pulses=%u lines sda=%s scl=%s\n", hal_status_text(status),
           bus->last_recover_pulses, level_text(sda_high), level_text(scl_high));
    return status == HAL_OK ? 0 : 1;
}

// A software reset on demand: what a panic or the watchdog would do, minus
// the fault. It exists to answer one question by eye -- does a software
// reset drop the switched 3V3 that powers the sensors, as pressing RST does?
// The blue LED sits on that rail (docs/hardware/board_notes.md, "Power rails
// and connectors"), and the answer decides whether the SCD41 is found idle
// or still measuring after such a reboot.
static int cmd_restart(void *context, int argc, char **argv)
{
    (void)context;
    (void)argc;
    (void)argv;
    printf("RESTART esp_restart() now -- watch the blue LED: dark for a moment = the sensors' "
           "3V3 drops as with RST; lit throughout = it stays up\n");
    fflush(stdout);
    usleep(RESTART_DRAIN_US);
    esp_restart();
    return 0;  // not reached
}

static esp_err_t register_commands(adp_i2c_t *bus)
{
    const esp_console_cmd_t scan = {
        .command = "i2c_scan",
        .help = "Probe every I2C address and compare with board_config.h",
        .func_w_context = cmd_i2c_scan,
        .context = bus,
    };
    const esp_console_cmd_t recover = {
        .command = "i2c_recover",
        .help = "Clock a stuck I2C slave free (up to nine SCL pulses, then STOP)",
        .func_w_context = cmd_i2c_recover,
        .context = bus,
    };
    const esp_console_cmd_t restart = {
        .command = "restart",
        .help = "Software reset (esp_restart): does the sensors' 3V3 drop, as with RST?",
        .func_w_context = cmd_restart,
        .context = NULL,
    };
    esp_err_t err = esp_console_cmd_register(&scan);
    if (err == ESP_OK) {
        err = esp_console_cmd_register(&recover);
    }
    if (err == ESP_OK) {
        err = esp_console_cmd_register(&restart);
    }
    return err;
}

hal_status_t console_start(adp_i2c_t *bus, const port_log_t *log)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = CONSOLE_PROMPT;
    const esp_console_dev_usb_serial_jtag_config_t dev_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_config, &repl_config, &repl);
    if (err == ESP_OK) {
        err = esp_console_register_help_command();
    }
    if (err == ESP_OK) {
        err = register_commands(bus);
    }
    if (err == ESP_OK) {
        err = esp_console_start_repl(repl);
    }

    const log_field_t fields[] = {
        log_str("prompt", CONSOLE_PROMPT),
        log_str("commands", "help i2c_scan i2c_recover restart"),
        log_i64("esp_err", (int64_t)err),
    };
    port_log_write(log, err == ESP_OK ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR, "console",
                   err == ESP_OK ? "console ready on USB-Serial-JTAG" : "console failed to start",
                   fields, sizeof(fields) / sizeof(fields[0]));

    return err == ESP_OK ? HAL_OK : HAL_ERR_IO;
}
