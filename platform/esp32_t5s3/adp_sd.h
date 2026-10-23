// SPDX-License-Identifier: Apache-2.0
//
// The microSD card behind port_files (docs/adr/0024-sd-card-recordings-and-
// debug-log.md): ESP-IDF's SD-over-SPI driver and FATFS (FAT12/16/32;
// exFAT stays off, as ESP-IDF builds it), mounted at /sdcard, the files
// reached through ESP-IDF's VFS with the same POSIX calls the host's
// adp_files_dir makes.
//
// No card is a normal state. The slot has no card-detect line the firmware
// can see (docs/hardware/board_notes.md, "The microSD slot"), so whoever owns
// the card -- apps/firmware_esp32/main/card_writer.c -- calls adp_sd_mount()
// on a slow timer and when asked at the console; a failed attempt returns a
// status and logs nothing, and with `quiet` set the attempt keeps ESP-IDF's
// own SD driver lines off the log too. A card that disappears under a
// mounted filesystem is found out by the next call that touches it: the
// card no longer answers its status command, the filesystem is unmounted,
// and that call and every later one say HAL_ERR_NOT_READY until a mount
// succeeds again. A file open across that keeps answering
// HAL_ERR_NOT_READY; close() releases it.
//
// The card can also be lent (docs/adr/0036-the-card-as-a-usb-disk.md): the
// filesystem unmounted, the card brought up again for its sectors alone, and
// those read and written for a USB host until adp_sd_end_lend(). While lent
// the port answers HAL_ERR_NOT_READY, as without a card, and no mount is
// made.
//
// Not thread-safe: one task owns the card and makes every call.

#ifndef HAC_PLATFORM_ESP32_T5S3_ADP_SD_H
#define HAC_PLATFORM_ESP32_T5S3_ADP_SD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "hal_status.h"
#include "port_files.h"
#include "port_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// The VFS path the card is mounted at.
#define ADP_SD_MOUNT_POINT "/sdcard"

// One more than the port's minimum, as FATFS's max_files.
#define ADP_SD_MAX_OPEN (PORT_FILES_MIN_OPEN + 1u)

typedef enum {
    ADP_SD_NOT_TRIED = 0,  // no mount attempted yet
    ADP_SD_MOUNTED,
    ADP_SD_NO_CARD,     // the last attempt found no card that answers
    ADP_SD_UNREADABLE,  // a card answers, but holds no FAT filesystem this mounts
    ADP_SD_LOST,        // was mounted; the card stopped answering
    ADP_SD_EJECTED,     // unmounted on request
    ADP_SD_LENT,        // its sectors lent to a USB host (adp_sd_lend())
} adp_sd_state_t;

struct port_file_s {
    bool used;
    int fd;               // -1 once the filesystem it was opened on is gone
    uint32_t generation;  // the mount it was opened on
    char path[PORT_FILES_PATH_MAX];
};

typedef struct {
    char name[8];  // the card's product name, from its CID
    bool sdhc;     // SDHC/SDXC (block-addressed), else SDSC
    uint64_t capacity_bytes;
    bool free_known;      // the free space was asked for, and FATFS answered
    uint64_t free_bytes;  // meaningful when free_known
    uint32_t freq_khz;    // the SPI clock the card runs at
} adp_sd_card_info_t;

typedef struct {
    port_files_t port;
    const port_log_t *log;

    adp_sd_state_t state;
    bool bus_ready;
    void *card;       // sdmmc_card_t *, owned by ESP-IDF while mounted, adp_sd.c's while lent
    int lent_device;  // the SD-over-SPI device while lent, else -1
    esp_err_t last_mount_err;
    uint32_t generation;  // incremented by every successful mount

    uint32_t mount_attempts;
    uint32_t mounts;
    // The slowest single call that waited on the card, since boot: what the
    // event loop would have waited, had it written itself.
    uint32_t slowest_write_us;
    uint32_t slowest_flush_us;

    struct port_file_s files[ADP_SD_MAX_OPEN];
} adp_sd_t;

// Brings up the SPI bus and MISO's pull-up. Touches no card. HAL_ERR_IO when
// the bus cannot be had.
hal_status_t adp_sd_init(adp_sd_t *sd, const port_log_t *log);

port_files_t *adp_sd_port(adp_sd_t *sd);

// Mounts the card's FAT filesystem -- never formats it. HAL_OK, or
// HAL_ERR_NOT_FOUND with no card answering, HAL_ERR_UNSUPPORTED for a card
// without a filesystem this mounts, HAL_ERR_IO otherwise. Mounted already:
// HAL_OK, nothing done. `quiet`: ESP-IDF's SD driver logs nothing during the
// attempt.
hal_status_t adp_sd_mount(adp_sd_t *sd, bool quiet);

// Unmounts: every open file's handle stays, answering HAL_ERR_NOT_READY
// until it is closed. The state becomes `why` (ADP_SD_EJECTED or
// ADP_SD_LOST).
void adp_sd_unmount(adp_sd_t *sd, adp_sd_state_t why);

bool adp_sd_mounted(const adp_sd_t *sd);

// The mounted card; with `with_free`, its free space too. That costs nothing
// once FATFS knows the count, and the first time after a mount on a card
// whose FSINFO sector does not hold it, a scan of the whole FAT: about 5 s on
// a 16 GB card at 20 MHz (seen 2026-10-16). HAL_ERR_NOT_READY without a card.
hal_status_t adp_sd_card_info(adp_sd_t *sd, bool with_free, adp_sd_card_info_t *out);

// Lends the card: every file's handle kept as adp_sd_unmount() keeps them,
// the filesystem unmounted, and the card initialised again for sector access
// -- what a mount does, without FATFS. With nothing mounted it looks for a
// card the same way. HAL_OK and the state ADP_SD_LENT; HAL_ERR_NOT_FOUND with
// no card answering, the state ADP_SD_NO_CARD; HAL_ERR_BUSY when lent
// already.
hal_status_t adp_sd_lend(adp_sd_t *sd);

// While lent: the card's sectors, and their size in bytes (512 for every SD
// card this slot takes). HAL_ERR_NOT_READY when not lent.
hal_status_t adp_sd_lent_geometry(const adp_sd_t *sd, uint32_t *sectors, uint32_t *sector_bytes);

// While lent: `count` sectors from `lba`. `buf` in internal RAM, as SPI DMA
// needs, 4-byte aligned. HAL_ERR_INVALID_ARG past the end; HAL_ERR_IO for a
// card that answers and failed, HAL_ERR_NOT_READY for one that no longer
// answers; HAL_ERR_NOT_READY when not lent.
hal_status_t adp_sd_lent_read(adp_sd_t *sd, uint32_t lba, void *buf, uint32_t count);
hal_status_t adp_sd_lent_write(adp_sd_t *sd, uint32_t lba, const void *buf, uint32_t count);

// Takes the card back: the SD-over-SPI device released, the state
// ADP_SD_NOT_TRIED, so the next adp_sd_mount() mounts what the host left.
// Not lent: nothing done.
void adp_sd_end_lend(adp_sd_t *sd);

// "not_tried", "mounted", "no_card", "unreadable", "lost", "ejected", "lent".
const char *adp_sd_state_name(adp_sd_state_t state);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_ADP_SD_H
