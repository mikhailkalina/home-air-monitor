// SPDX-License-Identifier: Apache-2.0
//
// The serial console: an esp_console REPL on the board's USB-Serial-JTAG
// port, the same one the log goes to, so whatever terminal shows the boot log
// can also type commands. Phase 2d's commands:
//
//   i2c_scan     probe every usable address and print what answered beside
//                what board_config.h expects there
//   i2c_recover  run the bus-clear procedure (port_i2c's recover()) and print
//                what it did
//
// Every line either command prints starts with "I2C_", so a pasted log can
// be grepped for the result whatever else the board was logging around it.

#ifndef HAC_APPS_FIRMWARE_ESP32_CONSOLE_H
#define HAC_APPS_FIRMWARE_ESP32_CONSOLE_H

#include "adp_i2c.h"
#include "hal_status.h"
#include "port_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// Scans `bus` and prints the report -- what i2c_scan prints. app_main() calls
// it once at boot, before console_start(), so the scan is in the log even if
// nobody types anything; the two never run at the same time.
void console_print_i2c_scan(adp_i2c_t *bus);

// Registers the commands and starts the REPL on its own task. `bus` and
// `log` are borrowed for the life of the firmware.
//
// Blocks its caller for about a second against a terminal that does not
// answer escape sequences (linenoise's probe runs in the caller's task; see
// start_console() in main.c), so call it where that costs nothing.
hal_status_t console_start(adp_i2c_t *bus, const port_log_t *log);

#ifdef __cplusplus
}
#endif

#endif  // HAC_APPS_FIRMWARE_ESP32_CONSOLE_H
