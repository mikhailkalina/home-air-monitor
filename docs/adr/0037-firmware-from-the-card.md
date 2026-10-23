# 37. Firmware from the card: an image in its root installed at the next mount, rolled back if it does not start

Date: 2026-10-23

## Status

Accepted, at the owner's request. Built and verified on the host (the image's
header and the verdict, `tests/unit/test_fw_image.c`); the firmware and its BSEC
variant build, with a new bootloader. Seen on the board 2026-10-23: an update
through the USB disk installed, restarted and confirmed itself, and the
slideshow's image was refused (`docs/hardware/board_notes.md`, open item
"Firmware from the card on the board", with what is still to see). Implements what ADR 0036's "For later: firmware through the disk"
listed; the network's OTA (architecture 10, phase 6) stays to do.

## Context

Since ADR 0036 the card is a USB disk a PC can write. The partition table has
held two OTA app slots and `otadata` since phase 2a, with no factory slot, so an
image can be written beside the one running and booted without esptool. The
owner asked for the simplest flow: a firmware file in the card's root, found
when the card is mounted, installed, then deleted -- and, when it comes from
the card, no version check at all.

## Decision

### The file

`home_air_monitor.bin` in the card's root (`FW_UPDATE_FILE_NAME`): the name the
build gives the image, the file esptool flashes at 0x20000 -- the one to copy,
from `build/` or `build-bsec/`. Copied through the USB disk (ADR 0036), or with
the card in a PC's reader.

### When

At **every mount** (`apps/firmware_esp32/main/card_writer.c`'s `on_mounted()`):
the boot's, `sd_mount`, the minute's quiet one, and the one after a USB disk
session -- the eject, then the update. On the card's task, which owns the card
(ADR 0024), so nothing else touches it meanwhile.

### What is checked: what the image is, never which version

`apps/firmware_esp32/main/fw_image.c`, pure and tested on the host, reads the
image's header and its app description -- at offset 32, where every ESP-IDF app
keeps it; `fw_update.c` holds the offsets to ESP-IDF's structs at compile time
-- and installs a file that is:

- **an ESP-IDF app image** (the header's magic, the description's), and not
  empty: a bootloader, a partition table, a zip are refused;
- **for this chip** (the header's chip id);
- **of this project** (`home_air_monitor`): the slideshow, the photo frame,
  each a project of its own, have no updater -- installed, they would leave
  only esptool to come back;
- **no larger than the slot**, 4 MB.

**No version check**, as the owner asked: an older image installs as readily
as a newer one, the same one again too, a `-dirty` build as well. Downgrading
is a feature here. ESP-IDF then verifies the whole image as it is written
(`esp_ota_end()`: the checksum and the appended SHA-256), which catches a file
truncated by an interrupted copy. The BSEC build and the plain one are the same
project and replace each other freely.

### How

`apps/firmware_esp32/main/fw_update.c`: the slot not running
(`esp_ota_get_next_update_partition()`), erased as it is written
(`OTA_WITH_SEQUENTIAL_WRITES`, a sector ahead, rather than one stretch of
seconds), the file read and written 4 KB at a time through a buffer in internal
RAM held only for the update, the card's task yielding every 64 KB; then
verification, **the file deleted**, the slot made the boot one, the log's lines
written out to the card, every file closed, the card unmounted, and a restart.
Deleted before the switch: a file that stayed would install again at the next
boot's mount, and again after that. A file that cannot be deleted stops the
update before the switch.

A file the update refuses -- another project's, another chip's, too large, not
an image, or one the verification rejects -- is **renamed**
`home_air_monitor.bin.rejected`, so no mount tries it again and whoever looks at
the card sees what happened; the log says why. One that could not be read or
written for any other reason stays, for the next mount. A power cut during the
write leaves the running image the boot one and the file on the card: the next
boot starts over.

The update takes seconds -- the file's read over SPI, the slot's erase and
write: 19.4 s for 1.45 MB on the board (2026-10-23), where 5-10 s was estimated.
Meanwhile the card's task records nothing (the event loop's rows wait in its
queue, the overflow counted) and each flash erase or write stalls the caches
for milliseconds -- a panel refresh under way may take a glitch, which the
restart's full refresh then clears.

### Rollback

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` in `sdkconfig.defaults` -- a bootloader
setting, so **the bootloader is new**, and an app one: `fw_update.c` refuses an
sdkconfig without it. The new image boots once as ESP-IDF's "pending verify"
and confirms itself at **its first frame** (`fw_update_confirm_running()`, the
event loop's first-frame hook): the boot, the switched rail, the sensors'
bring-up, the card and a full refresh behind it. One that resets before that --
a crash, a watchdog, a power cut -- the bootloader rolls back to the image it
replaced, and every boot after says so (`the other slot holds an update that did
not start`). Not covered: an image that reaches its first frame and then cannot
update itself again; esptool is the way back from that.

Two consequences of ESP-IDF's rules: while the running image is itself an
update not yet confirmed -- its first four seconds -- a new one cannot be
written (`ESP_ERR_OTA_ROLLBACK_INVALID_STATE`), and the boot's mount comes
before the first frame, so a file found then waits for the next mount; and a
brownout in that first boot -- this board's known one, at `PWR_EN`'s rise
(board notes) -- would roll the update back. The restart into an update is a
software reset, which keeps `PWR_EN` up, so that brownout is not expected
there.

An image flashed by esptool with `ota_data_initial.bin` boots from `ota_0` and
needs no confirmation: the bootloader, finding `otadata` empty and no factory
slot, writes an entry for `ota_0` itself, `valid` -- what the board's first boot
reported (2026-10-23), where this ADR first expected no OTA state at all
("flashed"). The test app, the slideshow and the calibration builds are
unaffected though they share the defaults.

### esptool from now on

The command stays the one `flash_args` gives -- the bootloader at 0x0, the
partition table at 0x8000, **`ota_data_initial.bin` at 0xf000**, the app at
0x20000 -- and flashing it once brings the new bootloader. `ota_data_initial.bin`
is no longer optional: once an update from the card has run, the board boots
`ota_1`, and an app flashed to 0x20000 alone would be written and never booted.
With it, `otadata` is reset and the board boots what esptool wrote.

### Versions

What an image says it is: its app description's `version`, which ESP-IDF fills
from `git describe --always --tags --dirty` of the tree it was built from --
today `8c73fdd-dirty`, the repository having no tags. It reaches the boot log,
`fw`, Home Assistant's `sw_version` (ADR 0033) and every update's log line,
`version=` and `from_version=`.

The scheme proposed, the owner's to start: **an annotated tag
`vMAJOR.MINOR.PATCH` at each merge to `main`** -- `v0.4.0` for the phases
merged so far, the minor stepping with each phase, the patch with fixes -- so an
image says `v0.4.0` built at the tag, `v0.4.0-3-g62944f7` three commits after,
and `-dirty` from a tree with uncommitted changes. The BSEC variant carries the
same version; `fw` and the boot log say which project, not which variant -- a
`+bsec` suffix, set in the BSEC build's CMake, would be the next step if it
matters.

What the versions are for: the card **compares none** (above). The network's
OTA, when it comes, should: offer only tagged, clean images, newer than the one
running by their tags -- the rule belongs in a pure function beside
`fw_update_judge()`, and to that ADR. Anti-rollback by ESP-IDF's
`secure_version` is not used: it burns eFuses, one way.

### The console

`fw`: each slot's project, version, build date and state (`flashed` -- no
readable `otadata` entry -- `new`,
`pending_verify`, `valid`, `invalid`, `aborted`), which boots next if that
differs, an update rolled back, and the last attempt since boot -- its result,
verdict, step, error, size and time.

## Consequences

- A new firmware reaches the board through the USB disk, without esptool and
  without a terminal: copy, eject, and it restarts into it.
- Any image of this project installs, older ones included; another project's is
  refused and renamed, never run.
- The board must be flashed once by esptool with the new bootloader, and from
  then on always with `ota_data_initial.bin`.
- An update that crashes or resets before its first frame comes back to the old
  image by itself; one that gets that far stays.
- Measured in the build against the commit before: image +18,436 bytes (ESP-IDF's
  `app_update` and the image verification), static internal RAM +392 bytes;
  bootloader 21,024 bytes, +80, of the 32 KB before the partition table.
- No port, no core change: one implementation, on this board.
