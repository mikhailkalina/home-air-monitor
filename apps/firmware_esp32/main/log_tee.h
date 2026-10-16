// SPDX-License-Identifier: Apache-2.0
//
// Everything ESP_LOG prints -- this project's port_log lines (adp_log.c goes
// through ESP_LOGx) and ESP-IDF's own -- also into a ring of lines in PSRAM
// (log_ring.h), from which card_writer.c writes the debug log to the card
// (docs/adr/0024-sd-card-recordings-and-debug-log.md).
//
// Installed with esp_log_set_vprintf(), so the hook runs in whatever task
// logged, and it never touches the card: it reserves a slot under a
// spinlock, formats the line into it, and hands the console exactly what it
// printed before. A full ring drops the line from the file only.
//
// What it does not see: the ROM's and the bootloader's lines, ESP-IDF's
// start-up before app_main(), ESP_EARLY_LOG and ESP_DRAM_LOG (they bypass
// vprintf), and the console's own printf() output (I2C_SCAN, TIME, SD, REC
// lines).
//
// The file's level is CONFIG_HAC_LOG_FILE_LEVEL, set apart from the
// console's (CONFIG_LOG_DEFAULT_LEVEL): a file more verbose than the console
// raises ESP-IDF's default level to it, and the hook keeps the extra lines
// off the console, so the console prints what it printed before.

#ifndef HAC_APPS_FIRMWARE_ESP32_LOG_TEE_H
#define HAC_APPS_FIRMWARE_ESP32_LOG_TEE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"

#include "hal_status.h"
#include "log_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

// CHOICE: a slot holds a 304-character line, past the longest port_log line
// (256, adp_log.c) with ESP-IDF's prefix; longer lines are cut, as one line.
#define LOG_TEE_SLOT_BYTES 320u

// CHOICE: 1024 lines, 320 KB of PSRAM (7.3 MB free after EPDiy's init). A
// boot logs about 150 lines, a quiet minute a handful: enough to keep the
// boot and some minutes after it for a card inserted later, and to ride out
// the writer waiting on a slow card.
#define LOG_TEE_SLOTS 1024u

// Allocates the ring in PSRAM and installs the hook. Call it first in
// app_main(), so the boot's own lines are in the file. HAL_ERR_NO_MEM, and no
// hook, without the memory.
hal_status_t log_tee_start(esp_log_level_t file_level);

// The ring, for its one consumer; NULL before log_tee_start() succeeded.
log_ring_t *log_tee_ring(void);

// The lines dropped since the last call, read under the producers' lock.
uint32_t log_tee_take_dropped(void);

// Holds the file's copy (true) and releases it (false): while held, the hook
// passes every line to the console and touches nothing in PSRAM, and counts
// what the file misses. main.c holds it while the switched 3V3 comes up:
// EPDiy logs a line within microseconds of raising PWR_EN, the rail's inrush
// dips VDD3V3, and an octal-PSRAM write in that dip is the best reading of
// the interrupt-watchdog resets the first phase-3f images met there
// (docs/hardware/board_notes.md, open item "A reboot loop when the switched
// 3V3 comes up").
void log_tee_hold(bool hold);

// The lines held off the file since the last call.
uint32_t log_tee_take_held(void);

#ifdef __cplusplus
}
#endif

#endif  // HAC_APPS_FIRMWARE_ESP32_LOG_TEE_H
