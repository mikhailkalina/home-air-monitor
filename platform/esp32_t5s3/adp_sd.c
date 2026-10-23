// SPDX-License-Identifier: Apache-2.0

#include "adp_sd.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sdkconfig.h"

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ff.h"

// ESP-IDF 5.5's sdmmc/include/sd_protocol_defs.h (lines 469-472, macros that
// shift an int into a size_t) fails -Wsign-conversion in every translation
// unit that includes it, through any of these four. Suppressed around them
// only, as adp_display.c does for EPDiy's header; the pragma is dead once a
// later ESP-IDF fixes the header.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include "diskio_sdmmc.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#pragma GCC diagnostic pop

#include "board_config.h"

// The recordings' names are long ("2026-10-16_bme68x_2.csv"): without FATFS's
// long file names they would not fit 8.3, and every open would fail at run
// time. A new choice in sdkconfig.defaults does not reach an sdkconfig that
// already exists (CLAUDE.md), so the build refuses rather than the card.
#if !CONFIG_FATFS_LFN_HEAP && !CONFIG_FATFS_LFN_STACK
#error "adp_sd needs FATFS long file names: CONFIG_FATFS_LFN_HEAP (sdkconfig.defaults) -- delete \
this build's sdkconfig, or run idf.py reconfigure, so the default reaches it"
#endif

#define TAG "adp_sd"

// ESP-IDF's own example value for SD over SPI: the largest DMA transfer, a
// sector and the protocol's overhead with room to spare.
#define ADP_SD_MAX_TRANSFER_BYTES 4000

// CHOICE: FAT's allocation unit when a card is formatted -- never here
// (format_if_mount_failed is false), so it only satisfies the API.
#define ADP_SD_ALLOCATION_UNIT_BYTES (16u * 1024u)

#define FULL_PATH_MAX (sizeof(ADP_SD_MOUNT_POINT) + PORT_FILES_PATH_MAX + 1u)
#define FATFS_PATH_MAX (8u + PORT_FILES_PATH_MAX)
#define MOUNT_MODE 0777
#define FILE_MODE 0666

// ESP-IDF's SD and FATFS log tags: what a mount attempt against an empty slot
// prints (sdmmc_card_init's command timeouts, the mount's failure line).
// Quietened during an automatic attempt only; the console's sd_mount shows
// them.
static const char *const SD_TAGS[] = {
    "sdmmc_common",       // the card's identification and initialisation
    "sdmmc_init",         // sdmmc_card_init()'s failure lines
    "sdmmc_sd",           // SD-specific commands
    "sdmmc_cmd",          // command errors
    "sdspi_host",         // SD over SPI: the transport
    "sdspi_transaction",  // its transactions' timeouts
    "vfs_fat_sdmmc",      // the mount's own "failed" line
    "diskio_sdmmc",       // FATFS's disk layer
};
#define SD_TAG_COUNT (sizeof(SD_TAGS) / sizeof(SD_TAGS[0]))

static adp_sd_t *self_of(port_files_t *port)
{
    return (adp_sd_t *)port->impl;
}

static sdmmc_card_t *card_of(const adp_sd_t *sd)
{
    return (sdmmc_card_t *)sd->card;
}

const char *adp_sd_state_name(adp_sd_state_t state)
{
    switch (state) {
        case ADP_SD_NOT_TRIED:
            return "not_tried";
        case ADP_SD_MOUNTED:
            return "mounted";
        case ADP_SD_NO_CARD:
            return "no_card";
        case ADP_SD_UNREADABLE:
            return "unreadable";
        case ADP_SD_LOST:
            return "lost";
        case ADP_SD_EJECTED:
            return "ejected";
        case ADP_SD_LENT:
            return "lent";
        default:
            return "?";
    }
}

bool adp_sd_mounted(const adp_sd_t *sd)
{
    return sd->state == ADP_SD_MOUNTED;
}

static bool full_path(const char *rel, char *out, size_t cap)
{
    const int n = snprintf(out, cap, "%s/%s", ADP_SD_MOUNT_POINT, rel);
    return n > 0 && (size_t)n < cap;
}

static uint32_t elapsed_us(int64_t since_us)
{
    const int64_t us = esp_timer_get_time() - since_us;
    return (us > 0 && us < (int64_t)UINT32_MAX) ? (uint32_t)us : 0u;
}

// A call the card refused in a way that may mean it is gone. A card that
// still answers its status command had a real error; one that does not has
// left the slot, and the filesystem goes with it.
static hal_status_t card_trouble(adp_sd_t *sd)
{
    if (sd->card != NULL && sdmmc_get_status(card_of(sd)) == ESP_OK) {
        return HAL_ERR_IO;
    }
    adp_sd_unmount(sd, ADP_SD_LOST);
    return HAL_ERR_NOT_READY;
}

static hal_status_t from_errno(adp_sd_t *sd, int err)
{
    switch (err) {
        case ENOSPC:
        // FatFs answers FR_DENIED -- EACCES -- when it cannot allocate a
        // cluster or a directory entry: a full card. Nothing else denies an
        // append-mode open, a mkdir or a write here: the slot has no
        // write-protect line.
        case EACCES:
            return HAL_ERR_NO_MEM;
        case ENOENT:
            return HAL_ERR_NOT_FOUND;
        case ENODEV:
        case EIO:
        case ENXIO:
        case ETIMEDOUT:
            return card_trouble(sd);
        default:
            return HAL_ERR_IO;
    }
}

static bool known_open(const adp_sd_t *sd, const port_file_t *file)
{
    for (size_t i = 0u; i < ADP_SD_MAX_OPEN; ++i) {
        if (&sd->files[i] == file) {
            return file->used;
        }
    }
    return false;
}

static bool stale(const adp_sd_t *sd, const port_file_t *file)
{
    return !adp_sd_mounted(sd) || file->fd < 0 || file->generation != sd->generation;
}

static port_file_t *open_by_path(adp_sd_t *sd, const char *path)
{
    for (size_t i = 0u; i < ADP_SD_MAX_OPEN; ++i) {
        if (sd->files[i].used && strcmp(sd->files[i].path, path) == 0) {
            return &sd->files[i];
        }
    }
    return NULL;
}

static void release(port_file_t *file)
{
    memset(file, 0, sizeof(*file));
    file->fd = -1;
}

// --- port_files ------------------------------------------------------------------

static hal_status_t sd_open_append(port_files_t *port, const char *path, port_file_t **out)
{
    adp_sd_t *sd = self_of(port);
    if (out == NULL || !port_files_path_ok(path)) {
        return HAL_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (!adp_sd_mounted(sd)) {
        return HAL_ERR_NOT_READY;
    }
    if (open_by_path(sd, path) != NULL) {
        return HAL_ERR_BUSY;
    }
    port_file_t *slot = NULL;
    for (size_t i = 0u; i < ADP_SD_MAX_OPEN && slot == NULL; ++i) {
        if (!sd->files[i].used) {
            slot = &sd->files[i];
        }
    }
    if (slot == NULL) {
        return HAL_ERR_BUSY;
    }

    char full[FULL_PATH_MAX];
    if (!full_path(path, full, sizeof(full))) {
        return HAL_ERR_INVALID_ARG;
    }
    const char *slash = strchr(path, '/');
    if (slash != NULL) {
        char dir[FULL_PATH_MAX];
        const size_t dir_len = sizeof(ADP_SD_MOUNT_POINT) + (size_t)(slash - path);
        memcpy(dir, full, dir_len);
        dir[dir_len] = '\0';
        if (mkdir(dir, MOUNT_MODE) != 0 && errno != EEXIST) {
            return from_errno(sd, errno);
        }
    }
    const int fd = open(full, O_WRONLY | O_CREAT | O_APPEND, FILE_MODE);
    if (fd < 0) {
        return from_errno(sd, errno);
    }
    slot->used = true;
    slot->fd = fd;
    slot->generation = sd->generation;
    memcpy(slot->path, path, strlen(path) + 1u);
    *out = slot;
    return HAL_OK;
}

static hal_status_t sd_append(port_files_t *port, port_file_t *file, const void *data, size_t len)
{
    adp_sd_t *sd = self_of(port);
    if (!known_open(sd, file) || (data == NULL && len > 0u)) {
        return HAL_ERR_INVALID_ARG;
    }
    if (stale(sd, file)) {
        return HAL_ERR_NOT_READY;
    }
    const int64_t start = esp_timer_get_time();
    const uint8_t *p = (const uint8_t *)data;
    size_t left = len;
    hal_status_t status = HAL_OK;
    while (left > 0u && status == HAL_OK) {
        const ssize_t n = write(file->fd, p, left);
        if (n < 0) {
            status = from_errno(sd, errno);
        } else if (n == 0) {
            // FatFs writes what fits and reports success: a short write is a
            // full card (ff.c, f_write: "Could not allocate a new cluster").
            status = HAL_ERR_NO_MEM;
        } else {
            p += n;
            left -= (size_t)n;
        }
    }
    const uint32_t us = elapsed_us(start);
    if (us > sd->slowest_write_us) {
        sd->slowest_write_us = us;
    }
    return status;
}

static hal_status_t sd_flush(port_files_t *port, port_file_t *file)
{
    adp_sd_t *sd = self_of(port);
    if (!known_open(sd, file)) {
        return HAL_ERR_INVALID_ARG;
    }
    if (stale(sd, file)) {
        return HAL_ERR_NOT_READY;
    }
    const int64_t start = esp_timer_get_time();
    const hal_status_t status = (fsync(file->fd) == 0) ? HAL_OK : from_errno(sd, errno);
    const uint32_t us = elapsed_us(start);
    if (us > sd->slowest_flush_us) {
        sd->slowest_flush_us = us;
    }
    return status;
}

static hal_status_t sd_close(port_files_t *port, port_file_t *file)
{
    adp_sd_t *sd = self_of(port);
    if (!known_open(sd, file)) {
        return HAL_ERR_INVALID_ARG;
    }
    if (stale(sd, file)) {
        release(file);
        return HAL_ERR_NOT_READY;
    }
    hal_status_t status = HAL_OK;
    if (fsync(file->fd) != 0) {
        status = from_errno(sd, errno);
    }
    // from_errno() may have unmounted the card, closing the descriptor.
    if (file->fd >= 0 && file->generation == sd->generation && adp_sd_mounted(sd) &&
        close(file->fd) != 0 && status == HAL_OK) {
        status = from_errno(sd, errno);
    }
    release(file);
    return status;
}

// FatFs's own directory reading: a name and a size in one pass, where the
// VFS's readdir() would want a stat() -- a directory search -- per file.
static hal_status_t sd_list(port_files_t *port, const char *dir, port_files_visit_fn visit,
                            void *ctx)
{
    adp_sd_t *sd = self_of(port);
    if (!port_files_dir_ok(dir) || visit == NULL) {
        return HAL_ERR_INVALID_ARG;
    }
    if (!adp_sd_mounted(sd)) {
        return HAL_ERR_NOT_READY;
    }
    char path[FATFS_PATH_MAX];
    (void)snprintf(path, sizeof(path), "%u:/%s", (unsigned)ff_diskio_get_pdrv_card(card_of(sd)),
                   dir);
    FF_DIR handle;
    FRESULT res = f_opendir(&handle, path);
    if (res == FR_NO_PATH || res == FR_NO_FILE) {
        return HAL_OK;
    }
    if (res != FR_OK) {
        return (res == FR_NOT_READY || res == FR_DISK_ERR) ? card_trouble(sd) : HAL_ERR_IO;
    }
    static FILINFO info;  // ~300 bytes with long names: not on the caller's stack
    for (;;) {
        res = f_readdir(&handle, &info);
        if (res != FR_OK || info.fname[0] == '\0') {
            break;
        }
        if ((info.fattrib & AM_DIR) == 0u) {
            visit(ctx, info.fname, (uint64_t)info.fsize);
        }
    }
    (void)f_closedir(&handle);
    if (res != FR_OK) {
        return (res == FR_NOT_READY || res == FR_DISK_ERR) ? card_trouble(sd) : HAL_ERR_IO;
    }
    return HAL_OK;
}

static hal_status_t sd_remove(port_files_t *port, const char *path)
{
    adp_sd_t *sd = self_of(port);
    if (!port_files_path_ok(path)) {
        return HAL_ERR_INVALID_ARG;
    }
    if (!adp_sd_mounted(sd)) {
        return HAL_ERR_NOT_READY;
    }
    if (open_by_path(sd, path) != NULL) {
        return HAL_ERR_BUSY;
    }
    char full[FULL_PATH_MAX];
    if (!full_path(path, full, sizeof(full))) {
        return HAL_ERR_INVALID_ARG;
    }
    return (unlink(full) == 0) ? HAL_OK : from_errno(sd, errno);
}

// --- the card ----------------------------------------------------------------------

hal_status_t adp_sd_init(adp_sd_t *sd, const port_log_t *log)
{
    memset(sd, 0, sizeof(*sd));
    for (size_t i = 0u; i < ADP_SD_MAX_OPEN; ++i) {
        release(&sd->files[i]);
    }
    sd->log = log;
    sd->lent_device = -1;
    sd->port.open_append = sd_open_append;
    sd->port.append = sd_append;
    sd->port.flush = sd_flush;
    sd->port.close = sd_close;
    sd->port.list = sd_list;
    sd->port.remove = sd_remove;
    sd->port.impl = sd;

    const spi_bus_config_t bus = {
        .mosi_io_num = BOARD_SD_MOSI_GPIO,
        .miso_io_num = BOARD_SD_MISO_GPIO,
        .sclk_io_num = BOARD_SD_SCLK_GPIO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = ADP_SD_MAX_TRANSFER_BYTES,
    };
    const esp_err_t err = spi_bus_initialize(BOARD_SD_SPI_HOST, &bus, SDSPI_DEFAULT_DMA);
    // MISO has no pull-up on the board (R14 is not fitted): the S3's own,
    // ~45 kOhm, keeps it from floating while no card drives it.
    const esp_err_t pull = gpio_set_pull_mode(BOARD_SD_MISO_GPIO, GPIO_PULLUP_ONLY);
    sd->bus_ready = (err == ESP_OK);

    const log_field_t fields[] = {
        log_i64("spi_host", (int64_t)BOARD_SD_SPI_HOST),
        log_i64("cs_gpio", BOARD_SD_CS_GPIO),
        log_i64("sclk_gpio", BOARD_SD_SCLK_GPIO),
        log_i64("mosi_gpio", BOARD_SD_MOSI_GPIO),
        log_i64("miso_gpio", BOARD_SD_MISO_GPIO),
        log_i64("esp_err", (int64_t)((err != ESP_OK) ? err : pull)),
    };
    port_log_write(log, (err == ESP_OK && pull == ESP_OK) ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR, TAG,
                   "spi bus for the card", fields, sizeof(fields) / sizeof(fields[0]));
    return sd->bus_ready ? HAL_OK : HAL_ERR_IO;
}

port_files_t *adp_sd_port(adp_sd_t *sd)
{
    return &sd->port;
}

static hal_status_t mount_status(esp_err_t err)
{
    switch (err) {
        case ESP_OK:
            return HAL_OK;
        case ESP_FAIL:  // a card that answers, without a filesystem FATFS mounts
            return HAL_ERR_UNSUPPORTED;
        case ESP_ERR_NO_MEM:
            return HAL_ERR_IO;
        default:  // ESP_ERR_TIMEOUT, ESP_ERR_INVALID_RESPONSE, ...: nobody there
            return HAL_ERR_NOT_FOUND;
    }
}

// The card's host and its SPI device, as both a mount and a lend bring it up.
static void card_config(sdmmc_host_t *host, sdspi_device_config_t *slot)
{
    const sdmmc_host_t host_default = SDSPI_HOST_DEFAULT();
    *host = host_default;
    host->slot = BOARD_SD_SPI_HOST;
    host->max_freq_khz = BOARD_SD_SPI_MAX_KHZ;
    const sdspi_device_config_t slot_default = SDSPI_DEVICE_CONFIG_DEFAULT();
    *slot = slot_default;
    slot->gpio_cs = BOARD_SD_CS_GPIO;
    slot->host_id = BOARD_SD_SPI_HOST;
}

hal_status_t adp_sd_mount(adp_sd_t *sd, bool quiet)
{
    if (adp_sd_mounted(sd)) {
        return HAL_OK;
    }
    if (sd->state == ADP_SD_LENT) {
        return HAL_ERR_BUSY;  // the host's until adp_sd_end_lend()
    }
    if (!sd->bus_ready) {
        return HAL_ERR_IO;
    }

    esp_log_level_t saved[SD_TAG_COUNT];
    if (quiet) {
        for (size_t i = 0u; i < SD_TAG_COUNT; ++i) {
            saved[i] = esp_log_level_get(SD_TAGS[i]);
            esp_log_level_set(SD_TAGS[i], ESP_LOG_NONE);
        }
    }

    sdmmc_host_t host;
    sdspi_device_config_t slot;
    card_config(&host, &slot);
    const esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = false,  // never: the card may hold someone's files
        .max_files = (int)ADP_SD_MAX_OPEN,
        .allocation_unit_size = ADP_SD_ALLOCATION_UNIT_BYTES,
        // Every FATFS call asks the card first, so one pulled out is found
        // at the next call rather than by a write into nothing.
        .disk_status_check_enable = true,
        .use_one_fat = false,
    };
    sdmmc_card_t *card = NULL;
    sd->mount_attempts++;
    const esp_err_t err = esp_vfs_fat_sdspi_mount(ADP_SD_MOUNT_POINT, &host, &slot, &mount, &card);

    if (quiet) {
        for (size_t i = 0u; i < SD_TAG_COUNT; ++i) {
            esp_log_level_set(SD_TAGS[i], saved[i]);
        }
    }

    sd->last_mount_err = err;
    const hal_status_t status = mount_status(err);
    if (status != HAL_OK) {
        sd->state = (status == HAL_ERR_UNSUPPORTED) ? ADP_SD_UNREADABLE : ADP_SD_NO_CARD;
        return status;
    }
    sd->card = card;
    sd->state = ADP_SD_MOUNTED;
    sd->generation++;
    sd->mounts++;
    return HAL_OK;
}

void adp_sd_unmount(adp_sd_t *sd, adp_sd_state_t why)
{
    if (!adp_sd_mounted(sd)) {
        if (why == ADP_SD_EJECTED && sd->state != ADP_SD_LENT) {
            sd->state = why;  // and no automatic mount until asked
        }
        return;
    }
    for (size_t i = 0u; i < ADP_SD_MAX_OPEN; ++i) {
        if (sd->files[i].used && sd->files[i].fd >= 0) {
            (void)close(sd->files[i].fd);
            sd->files[i].fd = -1;
        }
    }
    (void)esp_vfs_fat_sdcard_unmount(ADP_SD_MOUNT_POINT, card_of(sd));
    sd->card = NULL;
    sd->state = why;
}

hal_status_t adp_sd_card_info(adp_sd_t *sd, bool with_free, adp_sd_card_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!adp_sd_mounted(sd)) {
        return HAL_ERR_NOT_READY;
    }
    const sdmmc_card_t *card = card_of(sd);
    memcpy(out->name, card->cid.name, sizeof(out->name) - 1u);
    out->sdhc = (card->ocr & SD_OCR_SDHC_CAP) != 0u;
    out->capacity_bytes = (uint64_t)card->csd.capacity * (uint64_t)card->csd.sector_size;
    out->freq_khz = (uint32_t)card->real_freq_khz;
    uint64_t total = 0u;
    uint64_t free_bytes = 0u;
    if (with_free && esp_vfs_fat_info(ADP_SD_MOUNT_POINT, &total, &free_bytes) == ESP_OK) {
        out->free_known = true;
        out->free_bytes = free_bytes;
    }
    return HAL_OK;
}

// --- the card lent to a USB host (docs/adr/0036) ------------------------------------

// The card while lent: what esp_vfs_fat_sdspi_mount() keeps for a mounted
// one, kept here since nothing of FATFS holds it.
static sdmmc_card_t s_lent_card;

hal_status_t adp_sd_lend(adp_sd_t *sd)
{
    if (sd->state == ADP_SD_LENT) {
        return HAL_ERR_BUSY;
    }
    if (!sd->bus_ready) {
        return HAL_ERR_IO;
    }
    // The files' handles stay, answering HAL_ERR_NOT_READY, as for an eject;
    // the unmount releases the SPI device the mount had made.
    adp_sd_unmount(sd, ADP_SD_EJECTED);

    sdmmc_host_t host;
    sdspi_device_config_t slot;
    card_config(&host, &slot);
    sdspi_dev_handle_t device = -1;
    // What esp_vfs_fat_sdspi_mount() does before FATFS: the host's init (a
    // no-op for SD over SPI), the card's device on the bus, the card's
    // identification and initialisation.
    esp_err_t err = (*host.init)();
    if (err == ESP_OK) {
        err = sdspi_host_init_device(&slot, &device);
    }
    if (err == ESP_OK) {
        host.slot = device;
        memset(&s_lent_card, 0, sizeof(s_lent_card));
        err = sdmmc_card_init(&host, &s_lent_card);
        if (err != ESP_OK) {
            (void)sdspi_host_remove_device(device);
        }
    }
    sd->last_mount_err = err;
    if (err != ESP_OK) {
        sd->state = ADP_SD_NO_CARD;
        return HAL_ERR_NOT_FOUND;
    }
    sd->card = &s_lent_card;
    sd->lent_device = device;
    sd->state = ADP_SD_LENT;
    return HAL_OK;
}

hal_status_t adp_sd_lent_geometry(const adp_sd_t *sd, uint32_t *sectors, uint32_t *sector_bytes)
{
    if (sd->state != ADP_SD_LENT) {
        return HAL_ERR_NOT_READY;
    }
    const sdmmc_card_t *card = card_of(sd);
    *sectors = (uint32_t)card->csd.capacity;
    *sector_bytes = (uint32_t)card->csd.sector_size;
    return HAL_OK;
}

static hal_status_t lent_range(const adp_sd_t *sd, uint32_t lba, uint32_t count)
{
    if (sd->state != ADP_SD_LENT) {
        return HAL_ERR_NOT_READY;
    }
    const uint32_t sectors = (uint32_t)card_of(sd)->csd.capacity;
    if (count == 0u || lba >= sectors || count > sectors - lba) {
        return HAL_ERR_INVALID_ARG;
    }
    return HAL_OK;
}

// A transfer the card failed: an error if it still answers its status
// command, gone if not -- as card_trouble() says of a mounted one.
static hal_status_t lent_trouble(const adp_sd_t *sd)
{
    return (sdmmc_get_status(card_of(sd)) == ESP_OK) ? HAL_ERR_IO : HAL_ERR_NOT_READY;
}

hal_status_t adp_sd_lent_read(adp_sd_t *sd, uint32_t lba, void *buf, uint32_t count)
{
    const hal_status_t range = lent_range(sd, lba, count);
    if (range != HAL_OK) {
        return range;
    }
    if (sdmmc_read_sectors(card_of(sd), buf, (size_t)lba, (size_t)count) != ESP_OK) {
        return lent_trouble(sd);
    }
    return HAL_OK;
}

hal_status_t adp_sd_lent_write(adp_sd_t *sd, uint32_t lba, const void *buf, uint32_t count)
{
    const hal_status_t range = lent_range(sd, lba, count);
    if (range != HAL_OK) {
        return range;
    }
    if (sdmmc_write_sectors(card_of(sd), buf, (size_t)lba, (size_t)count) != ESP_OK) {
        return lent_trouble(sd);
    }
    return HAL_OK;
}

void adp_sd_end_lend(adp_sd_t *sd)
{
    if (sd->state != ADP_SD_LENT) {
        return;
    }
    (void)sdspi_host_remove_device(sd->lent_device);
    sd->lent_device = -1;
    sd->card = NULL;
    sd->state = ADP_SD_NOT_TRIED;
}
