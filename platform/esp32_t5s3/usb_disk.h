// SPDX-License-Identifier: Apache-2.0
//
// The microSD card as a USB disk (docs/adr/0036-the-card-as-a-usb-disk.md):
// the S3's one internal USB PHY taken from USB-Serial-JTAG -- the console,
// the log, esptool's port -- and given to the USB-OTG controller, where
// TinyUSB presents the card's sectors to the host as a mass-storage device;
// then given back to USB-Serial-JTAG, without a reset. The host sees the
// serial port go and a disk come, and the other way round.
//
// No port: one implementation, on this board. The card is lent by adp_sd
// (adp_sd_lend()) before usb_disk_start(), and taken back after
// usb_disk_stop(); in between the card's sectors are read and written only
// from inside usb_disk_step(), on the task that calls it -- the card's task,
// apps/firmware_esp32/main/card_writer.c -- so the card keeps one owner.
// When the session says the host is done (usb_disk_pure.h), the caller stops.
//
// Not thread-safe: one task makes every call.

#ifndef HAC_PLATFORM_ESP32_T5S3_USB_DISK_H
#define HAC_PLATFORM_ESP32_T5S3_USB_DISK_H

#include <stdbool.h>
#include <stdint.h>

#include "adp_sd.h"
#include "hal_status.h"
#include "usb_disk_pure.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sessions;  // started since boot
    bool active;
    // The last session's, or the current one's so far.
    usb_disk_verdict_t verdict;  // why it ended; USB_DISK_KEEP while active
    uint64_t lasted_ms;
    uint32_t configurations;  // how often a host configured the disk
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint32_t io_errors;  // sectors the card failed, the host told so
    uint32_t refused;    // reads and writes while the medium was not present
    uint32_t slowest_read_us;
    uint32_t slowest_write_us;
    uint32_t card_sectors;
} usb_disk_stats_t;

// Takes the PHY from USB-Serial-JTAG and starts TinyUSB with `sd`, which
// must be lent (adp_sd_lend()), behind its one logical unit. HAL_OK; else
// HAL_ERR_NOT_READY for a card not lent, HAL_ERR_BUSY when started already,
// HAL_ERR_IO when the PHY or TinyUSB would not start -- in which case
// USB-Serial-JTAG has the PHY again.
hal_status_t usb_disk_start(adp_sd_t *sd, uint64_t now_ms);

// Runs TinyUSB until its next event or `timeout_ms`, the host's reads and
// writes with it, and feeds the session.
void usb_disk_step(uint32_t timeout_ms, uint64_t now_ms);

// The button pressed: the medium gone at once, the session's verdict BUTTON.
void usb_disk_press(void);

// The session's verdict now (usb_disk_pure.h). USB_DISK_KEEP when stopped.
usb_disk_verdict_t usb_disk_verdict(uint64_t now_ms);

// Stops TinyUSB and gives the PHY back to USB-Serial-JTAG. The card stays
// lent: the caller takes it back (adp_sd_end_lend()). Stopped: nothing done.
void usb_disk_stop(uint64_t now_ms);

bool usb_disk_active(void);
void usb_disk_stats(usb_disk_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_USB_DISK_H
