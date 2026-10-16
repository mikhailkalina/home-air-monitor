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
//
// Phase 3e's (docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md),
// every line starting "TIME":
//
//   time         the system time in UTC and local time and where it came
//                from -- or, without one, why not -- and what the RTC holds
//   time_set <YYYY-MM-DDTHH:MM:SSZ>
//                UTC, so summer time leaves nothing ambiguous: writes the RTC
//                (which clears its VL bit) and sets the system time. Until
//                phase 4's SNTP, this is how the clock gets set.

#ifndef HAC_APPS_FIRMWARE_ESP32_CONSOLE_H
#define HAC_APPS_FIRMWARE_ESP32_CONSOLE_H

#include "adp_clock.h"
#include "adp_i2c.h"
#include "domain/civil_time.h"
#include "hal_status.h"
#include "port_log.h"
#include "wall_clock.h"

#ifdef __cplusplus
extern "C" {
#endif

// Scans `bus` and prints the report -- what i2c_scan prints. app_main() calls
// it once at boot, before console_start(), so the scan is in the log even if
// nobody types anything; the two never run at the same time.
void console_print_i2c_scan(adp_i2c_t *bus);

typedef struct {
    adp_i2c_t *bus;
    adp_clock_t *clock;       // the system time time_set sets
    const time_zone_t *zone;  // the local time `time` prints
    const char *zone_name;    // what `time` calls it: "Vilnius"
    // Where the system time came from: main.c's at boot, time_set's after.
    // Only the console's task touches it once the console has started.
    wall_clock_source_t *wall_source;
    const port_log_t *log;
} console_deps_t;

// Registers the commands and starts the REPL on its own task. `deps` is
// copied; what it points to is borrowed for the life of the firmware.
//
// Blocks its caller for about a second against a terminal that does not
// answer escape sequences (linenoise's probe runs in the caller's task; see
// start_console() in main.c), so call it where that costs nothing.
hal_status_t console_start(const console_deps_t *deps);

#ifdef __cplusplus
}
#endif

#endif  // HAC_APPS_FIRMWARE_ESP32_CONSOLE_H
