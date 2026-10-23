# 36. The card as a USB disk: the PHY lent from USB-Serial-JTAG to TinyUSB's mass storage

Date: 2026-10-22

## Status

Accepted, at the owner's request. Built and verified on the host (the
decisions, `tests/unit/test_usb_disk_pure.c`); the firmware, its BSEC variant,
the test app and the slideshow build. **On the board, 2026-10-23**: a session
entered and ended by the button, the PC reading the card, the PHY back to
USB-Serial-JTAG and the card mounted again without a reset; with a terminal
attached, the terminal reopening COM17 afterwards resets the chip -- a reset the
USB host asks for, which the S3 cannot refuse (`docs/hardware/board_notes.md`,
open item "The card as a USB disk on the board", for that and what is still to
see). Firmware
updates through the disk are not part of it: "For later" below records what
they will need.

## Context

The board's USB-C goes straight to the ESP32-S3's own USB pins, GPIO19 and
GPIO20 -- there is no USB-UART bridge: the console, the log and esptool's
COM17 are the S3's USB-Serial-JTAG controller. The S3 has a second USB
controller on the same pins, USB-OTG, which TinyUSB drives as any USB device;
a mass-storage device over the microSD card would let a PC copy the
recordings and the log (`data/`, `log/`, ADR 0024) without opening the case,
and later put a firmware image on the card.

The two controllers share the S3's one internal full-speed PHY; the board has
no external one. Only one of them is on the cable at a time.

Read in ESP-IDF 5.5, not seen on the board: the PHY is assigned by
`RTCCNTL.usb_conf.sw_usb_phy_sel` (`hal/esp32s3/include/hal/usb_wrap_ll.h`,
`usb_serial_jtag_ll.h`). `usb_new_phy()` with `USB_PHY_CTRL_OTG` gives it to
USB-OTG; `usb_del_phy()` leaves it there; `usb_new_phy()` with
`USB_PHY_CTRL_SERIAL_JTAG`, or installing the USB-Serial-JTAG driver, gives it
back (`components/usb/usb_phy.c:363`, `usb_serial_jtag.c:213`). The register
is in the RTC domain: a software reset keeps it, and nothing in ESP-IDF's
start-up writes it.

## Decision

### Two modes, one at a time

Normal operation stays exactly as it is: USB-Serial-JTAG on the PHY, the
console, the log, COM17 for esptool. **The USB disk is a mode asked for**,
in which the PHY is USB-OTG's and TinyUSB presents the card; when it ends the
PHY goes back to USB-Serial-JTAG, **without a reset**. The PC sees the serial
port go and a disk come, and the other way round.

Not taken: TinyUSB all the time, as a composite device with a CDC serial port
beside the disk. It would keep a console and the disk together, and lose what
this board's work has leaned on most: the ROM's and the panic handler's
output (backtraces, `BOD: Brownout detector was triggered`, the `rst:` lines
in the open item "A reboot loop when the switched 3V3 comes up"), which only
USB-Serial-JTAG carries; JTAG; and esptool's automatic reset into the
bootloader through COM17. A serial port that exists only while the firmware
runs is no help when the firmware does not.

### The switch

`platform/esp32_t5s3/usb_disk.c`. Into the mode: `usb_new_phy()` for USB-OTG,
device, full speed, then `tusb_init()`. Out: `tusb_deinit()` -- the
controller's pull-up off, the host sees the disk unplugged --
`usb_del_phy()`, and `usb_new_phy()` for USB-Serial-JTAG, whose pull-up then
shows the host the serial port again. The second session gives up the
USB-Serial-JTAG handle the first one made before taking the PHY again.

Because the register survives a software reset, a reboot while the disk is
up -- a panic, the watchdog -- comes back with the PHY still on USB-OTG: that
boot's ROM, bootloader and early log lines reach no terminal, and the serial
port returns only when the console installs its USB-Serial-JTAG driver, late
in the boot. Read, not seen. RST, alone or with BOOT for the ROM's download
mode, resets the whole chip and always gives USB-Serial-JTAG the PHY: the
board can always be recovered from the outside.

### TinyUSB itself, not esp_tinyusb

TinyUSB as Espressif packages it for ESP-IDF, `espressif/tinyusb` 0.21.0~2
(MIT), pinned exactly in `components/hac_platform/idf_component.yml`, with this
project's own configuration (`platform/esp32_t5s3/tinyusb/tusb_config.h`:
device, full speed, mass storage and nothing else), descriptors and SCSI
callbacks.

Not Espressif's `esp_tinyusb` layer above it (Apache-2.0, read at 2.4.0): its
mass-storage support is built around FATFS -- it mounts the medium for the
application itself, switches it between the application and the host on its
own when the host connects and disconnects, and formats a medium without a
filesystem unless told not to. Here `adp_sd` owns the card and the
filesystem and never formats (ADR 0024); two owners of one card would be the
bug. What `esp_tinyusb` would have given beyond that -- the PHY's set-up, a
task running `tud_task()` -- is a few lines.

### The card keeps one owner

The card's task (`apps/firmware_esp32/main/card_writer.c`, ADR 0024) owns the
card, and it owns the disk too: lent, it runs TinyUSB's `tud_task_ext()` in
place of its own wait -- `USB_DISK_STEP_MS`, returned from at once when an
event comes -- so the host's reads and writes happen inside TinyUSB's
callbacks on that task, and its own queue, the console's requests and the
button are looked at between them. No other task touches the card, and no
task is added.

`adp_sd` lends the card (`adp_sd_lend()`): every file closed as for an eject,
the filesystem unmounted, and the card brought up again for its sectors alone
-- the SD-over-SPI device and `sdmmc_card_init()`, what a mount does before
FATFS. While lent the port answers `HAL_ERR_NOT_READY` as with no card, and
no mount is made, the minute's automatic one included. `adp_sd_end_lend()`
releases the device; the card's task then mounts the card again, logging
ESP-IDF's lines this time -- the host may have left it unreadable.

### What the host gets

The whole card, its sectors as they are, readable and writable: one logical
unit, removable, 512-byte sectors, TinyUSB's 4 KB buffer
(`USB_DISK_MSC_BUFFER_BYTES`) read and written by one multi-block SD command
each, straight into TinyUSB's static buffer in internal RAM where the SPI DMA
reaches. SYNCHRONIZE CACHE has nothing to do -- a write is on the card when
its callback returns. A sector the card fails is reported to the host as a
medium error; a card that no longer answers ends the session.

What the host does with the card is the host's: it may delete recordings,
write files, or -- if Windows finds it unreadable and offers -- format it.
`adp_sd` still never formats.

### How it starts and how it ends

Asked for by the console's `usb_disk` or by the board's button (IO21, `S4`)
held for `USB_DISK_BUTTON_HOLD_MS`, 2 s -- the button's first use. The card is
made ready first -- the ring's log lines written out, the files closed, the
card lent -- and the console answers; the PHY switches
`USB_DISK_SWITCH_DELAY_MS`, 300 ms, later, so that the answer still reaches
the terminal. With no card the request is refused and nothing switches.

The session ends (`platform/esp32_t5s3/usb_disk_pure.c`, tested on the
host), the first of these that holds:

- the card failed and no longer answers;
- the button pressed;
- **the host ejected the medium** -- SCSI START STOP UNIT with LoEj set and
  Start clear: Explorer's "Eject", `eject` on Linux, macOS's -- the way it is
  meant to end; the medium is "not present" from that command on;
- no host configured the device within `USB_DISK_HOST_WAIT_MS`, 30 s: a
  charger, a power-only cable;
- a host configured it and is gone -- unconfigured, and not configured again
  within `USB_DISK_HOST_GONE_MS`, 3 s: a cable pulled while the battery runs
  the board, a PC shut down;
- the bus suspended for `USB_DISK_SUSPEND_MS`, 10 s, while configured: a PC
  asleep, or Windows' "Safely Remove Hardware", which stops a device without
  the eject Explorer sends -- to see on the board.

Not ended by time while a host holds the disk: the card stays the PC's until
the PC lets it go or the button is pressed. Pulling the cable on USB power
alone is a power cut, which the card has been through (ADR 0024).

### Meanwhile

Measuring, the screen, Wi-Fi and MQTT go on as before; the screen says
nothing of the mode -- the diagnostics screen's recorder shows `off`, as with
no card. The recorder's rows are dropped and counted, as with no card --
about 12 of the SCD41's and 20 of BSEC's a minute -- and the log stays in its
ring, 1,024 lines, the overflow counted in the file once the card is back. The
log's lines of the session reach no terminal; they reach the card after it.
After the session the recorder and the log open new files, the day's next
`_N`, as after any mount.

`sd` at the console prints the last session (`USB_DISK last of N: ended
<why> after S s, ... read KB, written KB, I/O errors, refused, slowest read
and write`), and the card's log has the same in its `the card is back from
USB` line.

### Identity

Espressif's vendor ID, 0x303A, and the product ID its `esp_tinyusb` gives a
TinyUSB device with mass storage alone, 0x4002 (`usb_disk_limits.h`): fine
for a device that is never sold; a product would ask Espressif for its own.
The serial number is the MAC, as USB-Serial-JTAG's is. Strings: "Home Air
Monitor", "Home Air Monitor card"; SCSI's inquiry "HAC", "microSD card". The
configuration asks for 500 mA: the board draws its running current from the
port whatever the PHY serves.

### What it costs

Measured in the build, against `main` with the same `sdkconfig` (a network
and a broker, without BSEC): static internal RAM **+5,536 bytes** (TinyUSB's
4,947 -- its 4 KB buffer, the stack's state -- `adp_sd`'s lent card and the
adapter's state), image **+25,556 bytes** (TinyUSB 14.6 KB of code, ESP-IDF's
`usb` component 2.7 KB). The stack: TinyUSB's callbacks and the SD driver run
on the card's task's 6 KB, whose low-water mark the task logs as before.
Throughput, the run-time memory and the stack's depth are the board's to
measure.

## For later: firmware through the disk

**Built since, by ADR 0037** (`docs/adr/0037-firmware-from-the-card.md`),
along these lines; what follows is kept as the record of what this branch
found. Not built here. What this branch found, for the phase that does it (architecture
10's OTA, phase 6):

- **The partitions are ready**: `partitions.csv` has `ota_0`, `ota_1` and
  `otadata` since phase 2a, no factory slot. No partition change is needed.
- **The flow**: an image copied to the card (say `update/home_air_monitor.bin`)
  while it is a disk; after the session, the card's task finds it, checks its
  `esp_app_desc_t` -- the project's name, so a slideshow or a calibration
  image is refused, and the chip -- writes it into the slot not running with
  `esp_ota_begin()`/`_write()`/`_end()`, which verify the image, renames the
  file so it is not applied twice, sets the boot partition and restarts.
- **A rollback**: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` -- a new bootloader,
  flashed once by esptool -- and the new image marking itself valid
  (`esp_ota_mark_app_valid_cancel_rollback()`) only once the first frame is on
  the glass and the sensors have been read, so an image that cannot get that
  far boots the old one again.
- **esptool after an update**: once `otadata` points at `ota_1`, an esptool
  flash of the app alone to 0x20000 (`ota_0`) is written and never booted.
  Every flash command must then carry `ota_data_initial.bin` at 0xF000 too,
  as `idf.py flash`'s `flash_args` does -- the instructions for flashing from
  Windows change with it.
- **Flash writes stall the caches**: while `esp_ota_write()` erases and
  writes, code and data in flash and PSRAM wait -- EPDiy's row output, the
  log's ring. The image is best applied with nothing else running, right
  before the restart.
- **Images built with credentials hold them** (CLAUDE.md): an image put on a
  card is the owner's own; never one from someone else, never a BSEC image
  shared.

Also left for later: the screen saying the card is lent (a core view field);
the recorder's rows kept in PSRAM through a session instead of dropped
(about 140 KB an hour); a read-only disk, should the owner want one; a
buffer larger than 4 KB, if the board's throughput asks for it.

## Consequences

- The recordings and the log come off the board over the cable, without the
  case opened or the card pulled.
- While the disk is up there is no console, no log on the terminal and no
  esptool on COM17: ending the session -- eject, or the button -- comes
  before flashing. A firmware hung in the mode is recovered with RST.
- A session costs the recorder its rows and the screen nothing.
- The button now does something: held two seconds it takes the card away
  from the recorder.
- TinyUSB is a new dependency, MIT (`THIRD_PARTY_NOTICES.md`), fetched as a
  managed component, linked only where called: the firmware. The test app,
  the slideshow and the photo frame compile it and leave it to the linker.
- No port and no core change: one implementation, on this board; the
  simulator's card is a directory on the PC already.
