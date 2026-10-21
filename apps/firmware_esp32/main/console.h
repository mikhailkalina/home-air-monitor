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
// Phase 3e's and 4f's (docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md
// and its amendment), every line starting "TIME":
//
//   time         the system time in UTC and local time and where it came
//                from -- or, without one, why not -- and what the RTC holds;
//                since phase 4f also what SNTP has said (TIME_SNTP lines):
//                its server, its answers, how far the system time and the
//                RTC were from the last one, the RTC's drift, the writes
//   time_set <YYYY-MM-DDTHH:MM:SSZ>
//                UTC, so summer time leaves nothing ambiguous: writes the RTC
//                (which clears its VL bit) and sets the system time. In an
//                image with a network, until SNTP's next answer sets both.
//   rtc_stop     (phase 4f) the RTC's STOP bit set: it stands still, and a
//                boot finds no time in it -- for checking that the network
//                alone gives the time. time_set or SNTP's next answer starts
//                it again.
//
// Phase 3f's (docs/adr/0024-sd-card-recordings-and-debug-log.md), every line
// starting "SD" or "REC", each answered by the card's own task
// (card_writer.h), so nothing here waits on the card itself:
//
//   sd           mounted or not -- and why not -- the card, its size and
//                what is free, and the files in data/ and log/
//   sd_mount     mount now, with ESP-IDF's SD driver lines shown
//   sd_eject     flush and close every file and unmount: safe to remove; no
//                automatic mount until sd_mount
//   rec          recording on or off, the file each source is written to,
//                rows and bytes since boot and what was dropped, the log
//                file's, and what the card costs the event loop
//
// Phase 3g's (docs/adr/0025-touch-input-gt911.md), every line starting
// "TOUCH":
//
//   touch        the controller: found or not, its address, product ID,
//                firmware and the configuration it holds -- resolution,
//                touch number, the interrupt's trigger, the checksum -- what
//                it has reported, and the last few events, raw and mapped
//                onto the panel: a finger at each corner is how the board's
//                swap and mirrors are checked
//   touch_sleep  the sleep command, INT held low: no touch until touch_wake,
//                or until a reset's init() wakes it
//   touch_wake   the wake pulse on INT -- or, for a controller never found,
//                a whole init() again
//
// Phase 4e's (docs/adr/0032-wifi-on-the-board.md), every line starting
// "WIFI":
//
//   wifi         the station: its state, the network's name, the signal, the
//                address and the gateway, the access point it joined; how
//                often the link came and went, the attempts that failed and
//                why the last one did, and when the next comes; the MAC and
//                the device's name on the network. The password never.
//   vbat         the battery's divider on GPIO14, an ADC2 input that Wi-Fi
//                shares: 32 reads, how many ADC2 refused, the pin's and the
//                battery's millivolts -- whether it reads at all with Wi-Fi
//                up (docs/hardware/board_notes.md, open item on GPIO14)

#ifndef HAC_APPS_FIRMWARE_ESP32_CONSOLE_H
#define HAC_APPS_FIRMWARE_ESP32_CONSOLE_H

#include "adp_clock.h"
#include "adp_i2c.h"
#include "adp_input.h"
#include "adp_net.h"
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
    // Where the system time came from: main.c's at boot, time_set's and the
    // time task's after (net_time.h). The event loop reads it, a word, for
    // the diagnostics screen.
    wall_clock_source_t *wall_source;
    adp_input_t *touch;     // the console's touch, touch_sleep and touch_wake
    adp_net_t *net;         // the console's wifi; NULL without a network in this image
    const char *device_id;  // "hac-..." from the station's MAC; "" when unread
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
