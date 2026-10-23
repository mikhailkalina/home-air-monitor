// SPDX-License-Identifier: Apache-2.0
//
// Every number the card's USB disk mode runs on (docs/adr/0036-the-card-as-a-
// usb-disk.md): the session's in usb_disk_pure.h, the adapter's in usb_disk.c,
// the card's task's in apps/firmware_esp32/main/card_writer.c, TinyUSB's in
// tinyusb/tusb_config.h. None is measured yet; each says what it is and which
// board check gives it a measured value (docs/hardware/board_notes.md, open
// item "The card as a USB disk").

#ifndef HAC_PLATFORM_ESP32_T5S3_USB_DISK_LIMITS_H
#define HAC_PLATFORM_ESP32_T5S3_USB_DISK_LIMITS_H

// CHOICE: from the console's answer to the PHY leaving USB-Serial-JTAG. The
// console prints its line before the port goes; a third of a second lets the
// line reach the terminal, which a switch at once would cut off.
#define USB_DISK_SWITCH_DELAY_MS 300u

// CHOICE: a disk no host has configured within this is given back. The mode
// is asked for from a PC at the other end of the cable, which enumerates a
// new device within a second or two; nothing configuring it for this long
// means a charger, a power-only cable, or a PC that is not there -- and the
// card would otherwise stay out of the recorder's reach until someone came.
#define USB_DISK_HOST_WAIT_MS 30000u

// CHOICE: a host that configured the disk and then unconfigured it -- a
// cable pulled while the board runs on its battery, a PC shut down -- is
// gone once this has passed without a new configuration. Long enough for a
// bus reset and re-enumeration, which a host may do while it keeps the disk,
// to come back first.
#define USB_DISK_HOST_GONE_MS 3000u

// CHOICE: a configured disk whose bus has been suspended this long is given
// back: a PC gone to sleep, or Windows' "Safely Remove Hardware", which
// stops the device without the eject Explorer's "Eject" sends. A host
// suspends an idle bus after 3 ms of silence by the USB specification, but
// none suspends a mass-storage device it holds open while it is awake.
#define USB_DISK_SUSPEND_MS 10000u

// CHOICE: how long the card's task lets TinyUSB wait for its next event
// before it looks at its own queue, the console and the button. TinyUSB
// returns as soon as an event comes, so this is not a delay on the disk.
#define USB_DISK_STEP_MS 50u

// CHOICE: TinyUSB's mass-storage buffer (CFG_TUD_MSC_EP_BUFSIZE): eight 512-
// byte sectors, read or written by one multi-block SD command each. A static
// buffer in internal RAM, paid in every image that links the mode -- the
// board's `idf.py size` gives the whole of TinyUSB's share. A larger one
// would cut the SD commands' overhead further; the board's figures for
// `read_kb_s=` and `write_kb_s=` say whether it is worth it.
#define USB_DISK_MSC_BUFFER_BYTES 4096u

// CHOICE: the button (BOARD_BUTTON_GPIO) held this long asks for the mode,
// pressed again gives the card back. Long, so a brush against the button
// does not take the card from the recorder.
#define USB_DISK_BUTTON_HOLD_MS 2000u

// CHOICE: a level the button holds for less than this is a bounce. The
// card's task samples the button every CARD_WRITER_TICK_MS, 250 ms; the
// 4.7 uF on the line (v2.4 schematic, `C13`) smooths the rest.
#define USB_DISK_BUTTON_DEBOUNCE_MS 50u

// Espressif's vendor ID and the product ID its esp_tinyusb component gives a
// TinyUSB device with mass storage alone (0x4000 with the MSC bit, 0x0002).
// Good for a device that is never sold: a product would ask Espressif for a
// PID of its own (https://github.com/espressif/usb-pids). The host binds its
// mass-storage driver by the interface's class, not by these.
#define USB_DISK_VID 0x303Au
#define USB_DISK_PID 0x4002u

// CHOICE: what the configuration descriptor asks of the host's port. The
// board draws its running current from the same port whatever the PHY
// serves -- Wi-Fi's peaks included -- so it declares the most a USB 2.0
// port gives.
#define USB_DISK_MAX_POWER_MA 500u

#endif  // HAC_PLATFORM_ESP32_T5S3_USB_DISK_LIMITS_H
