// SPDX-License-Identifier: Apache-2.0
//
// The decisions the card's USB disk mode makes (docs/adr/0036-the-card-as-a-
// usb-disk.md), kept free of ESP-IDF and TinyUSB headers so the host build
// can test them (tests/unit/test_usb_disk_pure.c), as adp_net_pure.c is.
// usb_disk.c feeds the session what TinyUSB reports, and the card's task
// (apps/firmware_esp32/main/card_writer.c) gives the card back when the
// session says so; the button's machine turns the level the task samples
// into the presses it acts on.
//
// The session: the card is lent to a host from usb_disk_session_start() until
// usb_disk_session_verdict() answers anything but USB_DISK_KEEP. It is given
// back, the first of these to hold winning:
//
//   CARD_FAILED  a sector the card would not read or write, and the card no
//                longer answering: there is nothing left to lend
//   BUTTON       the button pressed while lent
//   EJECTED      the host ejected the medium (SCSI START STOP UNIT with
//                LoEj set and Start clear): Explorer's "Eject", Linux's
//                eject(1), macOS's -- the way the disk is meant to end
//   NO_HOST      no host configured the device within host_wait_ms
//   HOST_GONE    a host configured it and then unconfigured it, and none
//                configured it again within host_gone_ms
//   SUSPENDED    configured, and the bus suspended for suspend_ms
//
// From the eject on, and from a press of the button, the medium is not
// present: a host still asking is told so (SCSI's "medium not present"),
// never handed a sector the card's task is about to take back.

#ifndef HAC_PLATFORM_ESP32_T5S3_USB_DISK_PURE_H
#define HAC_PLATFORM_ESP32_T5S3_USB_DISK_PURE_H

#include <stdbool.h>
#include <stdint.h>

#include "usb_disk_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    USB_DISK_KEEP = 0,  // the host keeps the card
    USB_DISK_BACK_CARD_FAILED,
    USB_DISK_BACK_BUTTON,
    USB_DISK_BACK_EJECTED,
    USB_DISK_BACK_NO_HOST,
    USB_DISK_BACK_HOST_GONE,
    USB_DISK_BACK_SUSPENDED,
} usb_disk_verdict_t;

typedef struct {
    uint32_t host_wait_ms;
    uint32_t host_gone_ms;
    uint32_t suspend_ms;
} usb_disk_session_config_t;

// usb_disk_limits.h's numbers.
usb_disk_session_config_t usb_disk_session_config_default(void);

typedef struct {
    usb_disk_session_config_t cfg;
    uint64_t started_ms;
    bool configured;       // by a host, now
    bool ever_configured;  // by a host, in this session
    uint64_t unconfigured_ms;
    bool suspended;
    uint64_t suspended_ms;
    bool ejected;
    bool button;
    bool card_failed;
    uint32_t configurations;  // how often a host configured it
} usb_disk_session_t;

void usb_disk_session_start(usb_disk_session_t *s, const usb_disk_session_config_t *cfg,
                            uint64_t now_ms);

// TinyUSB's tud_mount_cb() and tud_umount_cb(): the host set a configuration,
// and the device left it -- a bus reset, a detach.
void usb_disk_session_configured(usb_disk_session_t *s, uint64_t now_ms);
void usb_disk_session_unconfigured(usb_disk_session_t *s, uint64_t now_ms);

// tud_suspend_cb() and tud_resume_cb().
void usb_disk_session_suspended(usb_disk_session_t *s, uint64_t now_ms);
void usb_disk_session_resumed(usb_disk_session_t *s);

// The host's START STOP UNIT: only LoEj with Start clear ejects. A stop
// without LoEj, or a load, changes nothing.
void usb_disk_session_start_stop(usb_disk_session_t *s, bool start, bool load_eject);

void usb_disk_session_button(usb_disk_session_t *s);
void usb_disk_session_card_failed(usb_disk_session_t *s);

// Whether a host may read and write the card now.
bool usb_disk_session_medium_present(const usb_disk_session_t *s);

usb_disk_verdict_t usb_disk_session_verdict(const usb_disk_session_t *s, uint64_t now_ms);

// "keep", "card_failed", "button", "ejected", "no_host", "host_gone",
// "suspended"; "?" for anything else.
const char *usb_disk_verdict_name(usb_disk_verdict_t verdict);

// --- the button ----------------------------------------------------------------------

typedef enum {
    USB_DISK_BUTTON_NONE = 0,
    USB_DISK_BUTTON_PRESS,  // a press began: once per press
    USB_DISK_BUTTON_HOLD,   // the same press has lasted hold_ms: once per press
} usb_disk_button_event_t;

typedef struct {
    uint32_t debounce_ms;
    uint32_t hold_ms;

    bool raw;  // the level last sampled: pressed or not
    uint64_t raw_since_ms;
    bool pressed;  // debounced
    bool hold_reported;
    bool ignore_until_release;
} usb_disk_button_t;

void usb_disk_button_init(usb_disk_button_t *b, uint32_t debounce_ms, uint32_t hold_ms);

// One sample: `raw_pressed` the level read at `now_ms`. Samples may come at
// any interval; a level counts once it has been read the same for
// debounce_ms, measured from the first sample that read it.
usb_disk_button_event_t usb_disk_button_sample(usb_disk_button_t *b, bool raw_pressed,
                                               uint64_t now_ms);

// The press under way has been acted on: nothing more of it -- no HOLD --
// is reported, and the next event is the next press's.
void usb_disk_button_consume(usb_disk_button_t *b);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_USB_DISK_PURE_H
