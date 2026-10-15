# 11. The I2C bus: `port_i2c`, an adapter over ESP-IDF's `i2c_master`, and a bus-clear of our own

Date: 2026-10-13

## Status

Accepted. Confirmed on the board 2026-10-13: the bus, the boot scan and the
whole `port_i2c` contract, forced hang and stall included (see
"Consequences").

## Context

Phase 2d brings up the board's one shared I2C bus (SDA GPIO18, SCL GPIO17:
the PCF8563 RTC, the touch controller and, from phase 3, the sensors) behind
`ports/port_i2c.h` (`docs/architecture.md` §5.4). Its brief: an adapter on
ESP-IDF 5.5's new `i2c_master` driver that maps every `esp_err_t` onto
`hal_status_t` and honours the per-call timeout; a `recover()` that clocks a
stuck slave free and reinitialises the bus; a console command that scans the
bus beside the addresses `board_config.h` expects; and a contract suite run on
the device against a known-present slave, covering an absent address, a
zero-length transfer, a timeout and recovery from a forced hang.

The port was a sketch in the architecture document. Writing its contract
meant deciding what it promises, and reading the driver
(`components/esp_driver_i2c/i2c_master.c`, `i2c_common.c`, and the S3's
`hal/esp32s3/include/hal/i2c_ll.h`) showed that a thin wrapper would keep
none of those promises:

1. **Every failed transfer is `ESP_ERR_INVALID_STATE`.**
   `s_i2c_transaction_start()` returns it whenever the status is not `DONE`
   -- an address NACK, a data NACK, a hardware SCL timeout, lost arbitration
   and the driver's own software timeout alike. Only `i2c_master_probe()`
   tells NACK (`ESP_ERR_NOT_FOUND`) from timeout (`ESP_ERR_TIMEOUT`).
2. **Every wait is `pdMS_TO_TICKS(xfer_timeout_ms)`, which rounds down.** At
   the board's 100 Hz tick a 1-9 ms timeout is 0 ticks: the driver starts the
   transfer, polls its event queue once without waiting, and reports failure
   while the hardware is still clocking -- the same truncation ADR 0006 found
   in `vTaskDelay()`.
3. **A zero-length transmit is `ESP_ERR_INVALID_ARG`.** `i2c_master_probe()`
   puts the same START, address+W, STOP on the wire.
4. **On the S3 the driver's clear-bus is unconfirmed.** `s_i2c_master_clear_bus()`
   sets `scl_rst_slv_en` for nine pulses, but on this chip
   `i2c_ll_master_is_bus_clear_done()` is a stub returning `false`, so nothing
   waits for the pulses, and `s_i2c_hw_fsm_reset()` resets the peripheral's
   registers straight afterwards. Whether any pulse reaches the wire is not
   established by reading the source. **Seen on the board (2026-10-13):** a
   transfer started while the RTC held SDA low completed without a single
   driver error, and SDA was already high when `recover()` ran next. On a
   freshly built bus the only thing that runs before a transfer's START is
   this clear-bus, triggered when the controller reports the bus busy -- so
   on this board it evidently does reach the wire, at least for a slave that
   needs a few clocks. `recover()` still does not rely on it: the driver runs
   it only when its own state happens to say "busy" or "timeout".
5. **The driver's own recovery is fragile.** It resets the controller (and
   clears the bus) before a transfer only while its status still says
   `TIMEOUT` -- and `i2c_master_probe()` ends by setting the status to `DONE`.
   After a NACK it busy-waits for the controller's bus-busy flag to clear,
   with no bound.

## Decision

### The port's promises (`ports/port_i2c.h`)

- 7-bit addresses; above `0x7F` is `HAL_ERR_INVALID_ARG`, nothing on the bus.
- **Zero-length:** `write(n = 0)` is the address-only probe -- `HAL_OK` if
  acknowledged, `HAL_ERR_NOT_FOUND` if not -- and is what a scan is built
  from. `read(n = 0)` is `HAL_ERR_INVALID_ARG`, because it cannot exist on the
  wire: a slave that acknowledged a read address already owns SDA. Same for
  `write_read()` with either length 0.
- **`to_ms` bounds the whole call**, waiting for the bus lock included, and
  must be at least 1. Honoured both ways: never give up early (a deadline
  shorter than a tick still completes a healthy transfer), never return later
  than `to_ms` plus the platform's scheduling granularity.
- **Errors say where to look:** `NOT_FOUND` is the device (absent, or busy and
  refusing its address); `IO` and `TIMEOUT` are the bus. A line stuck by a
  slave shows as `TIMEOUT` or `IO` depending on where the controller notices
  -- never `NOT_FOUND`, which would send a caller looking for an absent device
  when `recover()` is what it needs.

### The adapter (`platform/esp32_t5s3/adp_i2c.c`)

- **One deadline per call.** `to_ms` becomes an absolute `esp_timer` deadline;
  the lock, the driver call and any follow-up probe each get only what is
  left, converted by `adp_i2c_driver_wait_ms()` -- whole ticks rounded up plus
  one, expressed so the driver's own `pdMS_TO_TICKS()` recovers at least that
  count (finding 2, ADR 0006's fix).
- **A failure is classified by one probe.** On `ESP_ERR_INVALID_STATE`, if
  time remains, the same address is probed: answers now -> `IO`; refused ->
  `NOT_FOUND`; probe times out -> `TIMEOUT`; no time left -> `TIMEOUT`
  (finding 1). That probe is the only extra bus traffic the adapter makes, and
  only on a failure.
- **After a `TIMEOUT` or `IO`, the next transfer resets the controller first**
  (`i2c_master_bus_reset()`), doing what the driver would have done had the
  classification probe not reset its status (finding 5).
- **Zero-length writes go through `i2c_master_probe()`** (finding 3).
- **Device handles are cached** per address, eight at most, the oldest
  evicted -- the board and phase 3 together need four.
- **A mutex** guards the adapter: the console task and, from phase 3, the
  event loop both reach this bus.
- **Internal pull-ups on**, at 100 kHz (`BOARD_I2C_SCL_HZ`, the rate the
  vendor's examples run at). Whether external pull-ups exist is not
  confirmed.
- The arithmetic and the classification live in `adp_i2c_pure.c`, free of
  ESP-IDF headers, and are unit-tested on the host from there, as
  `duration_stats.c` is.

### `recover()`: the bus-clear, bit-banged

The controller is torn down, the two pins become open-drain GPIOs, and the
procedure of UM10204 §3.1.16 runs in the form ESP-IDF's software fallback and
Linux's `i2c_generic_scl_recovery()` share: SCL low first (if a slave is
holding an acknowledge with SCL high, that falling edge releases it); then,
while SDA is still low, one more clock at a time, **up to nine**; then a STOP
-- SDA low with SCL low, SCL up, SDA up. SDA is sampled with SCL low and the
STOP needs only a rising edge, so the stuck slave never sees one falling edge
too many. Then the controller is rebuilt.

It stops as soon as SDA is free rather than always clocking nine times. A
slave that was *receiving* when the bus stuck lets go after one clock;
eight more would clock `0xFF` into whatever register it was being written.

`i2c_master_bus_reset()` was not used for this. Its clear-bus is unconfirmed
on the S3 (finding 4), it reports nothing about the lines, and the brief asked
for nine pulses this code drives. `recover()` logs the line levels before and
after and how many pulses it took, so a pasted log shows what happened. For
the same reason the controller teardown inside `recover()` does not reset the
controller first unless the driver refuses to remove a device: an
unconditional reset would run the controller's clear-bus before `recover()`'s
own pulses, and the log would credit the wrong mechanism.

### The contract, and how a fault is made on purpose

`tests/contract/port_i2c_contract.c` runs on the host against
`platform/host/adp_i2c_fake.c` -- a register-file device model, the second
implementation the port rule asks for -- and in the test app against
`adp_i2c` with the RTC as the known-present slave and `0x08` (the SMBus host
address) as the absent one. A port cannot break its own bus, so the runner
supplies the faults:

- **Hang:** `adp_i2c_inject_stuck_slave()` bit-bangs START and the RTC's
  address with the read bit, clocks its acknowledge onto SDA and stops with
  SCL released high. The RTC keeps SDA low waiting for a falling edge that
  never comes -- the state a reset in the middle of a read leaves. The
  controller is then rebuilt with the slave still holding SDA, and the suite
  checks SDA is really low before relying on it. One round recovers with
  nothing in between, so a released SDA is `recover()`'s doing; a second
  round transfers on the hung bus first, to check that call returns promptly
  and does not blame the device.
- **Stall:** `adp_i2c_hold_scl_low()` routes the plain GPIO output to SCL in
  place of the controller's and drives it low, while the controller keeps
  reading the pin -- a slave stretching the clock forever, as the controller
  sees it. `adp_i2c_release_scl()` reconnects the controller's output without
  rebuilding anything, so the transfer after the stall meets whatever state
  the timeout left.

The fault functions are compiled into every build; the linker drops them from
the firmware image (checked: `inject stuck slave` is absent from
`home_air_monitor.bin` and present in `hac_test_app.bin`).

The host runner also runs the suite against five deliberately broken
wrappers -- a `recover()` that does nothing, a timeout that overruns, a
rejected zero-length write, a sub-tick deadline that fails early, a hung bus
reported as `NOT_FOUND` -- and asserts the matching check fails, so the suite
is known to catch each.

**Not run at boot**, unlike `port_clock` and `port_log` (ADR 0005, 0009): it
hangs and stalls the shared bus on purpose. It runs on demand in the test app.

### The scan and the console

`apps/firmware_esp32/main/i2c_scan.c` probes `0x08`-`0x77` with zero-length
writes, stops at the first probe that neither answers nor is refused (a dead
bus would otherwise repeat one timeout 112 times), and merges the result with
an expected-address table into rows: `found`, `MISSING`, `absent` (a candidate
that did not answer), `UNEXPECTED`, `not scanned`. Pure over the port, tested
on the host against the fake. (Since 2026-10-16 `console.c` does not print the
`absent` rows: a silent candidate says no more than an address the table never
names. The report still produces them.)

`console.c` runs ESP-IDF's `esp_console` REPL on the USB-Serial-JTAG port the
log already uses, with `i2c_scan` and `i2c_recover`; the firmware also runs the
scan once at boot, so the result is in the log without typing anything. Every
line starts `I2C_`; the last is
`I2C_SCAN_RESULT answered=<n> missing=<n> unexpected=<n> scan=complete|INCOMPLETE`.

**The touch address is in doubt, and the scan is built to settle it.**
`board_notes.md` recorded the touch controller at `0x5A` and "not a GT911",
from `src/touch.h` in the vendor repository. Phase 2d found that nothing in
that repository at the cited commit uses that header, while the vendor's S3
examples (`examples/touch/touch.ino`, `examples/demo/demo.ino`) drive a GT911
and look for it at `0x14` and `0x5D`. The recorded fact was not overwritten on
that evidence alone: the first scan listed `0x5A` as required and the two
GT911 addresses as candidates, side by side, so the board could answer.

**It answered `0x5D`** (2026-10-13): `0x51` and `0x5D` found, `0x5A` and
`0x14` silent. `BOARD_TOUCH_I2C_ADDRESS` is now `0x5D`, with `0x14` kept as
`BOARD_TOUCH_ALT_I2C_ADDRESS` -- a GT911 latches one or the other at power-up
and the vendor probes both -- and the scan's table follows: the touch address
required, the alternative a candidate, `0x5A` gone (anything answering there
now prints as `UNEXPECTED`). An address-only probe cannot name the part; the
GT911's product ID register can, and is left to the touch driver.

## Consequences

- **Verified on the host:** `test_port_i2c_contract` (the fake passes all 39
  contract checks; each of the five broken wrappers is caught by the check
  meant for it), `test_adp_i2c_pure`, `test_i2c_scan`. The firmware and the
  test app build with the project's full warning set.
- **Verified on the board, 2026-10-13: the bus and the scan.** The adapter
  brought the bus up with both lines idle-high, the boot scan completed over
  all 112 addresses (`I2C_SCAN_RESULT answered=2 missing=1 unexpected=0
  scan=complete` -- the one "missing" being the `0x5A` this ADR then
  retired), and `app_main` returned normally on its 3,584-byte stack. The
  bus and the console cost about 14 KB of internal RAM
  (`docs/hardware/board_notes.md`, "Free PSRAM at runtime").
- **Verified on the board, 2026-10-13: the contract.** The test app (ELF
  SHA256 `5f61e780d...`), run twice in a row with identical results:
  `3 Tests 0 Failures 0 Ignored`, every `contract port_i2c adp_i2c` line
  PASS, the whole `port_i2c` suite in about 150 ms. What the log shows
  beyond PASS:
  - **The forced hang is real.** Both times the RTC acknowledged the
    bit-banged read and still held SDA low after the rebuilt controller took
    the pins back (`inject stuck slave ... acked=true
    sda_held_after_handover=true`).
  - **`recover()` freed it itself, in two clocks**
    (`recover sda_was_high=false ... pulses=2 sda_high=true`), with nothing
    else touching the bus in between.
  - **A transfer on the re-hung bus completed** -- no driver error lines,
    and `recover()` afterwards found SDA already high (`pulses=0`). The
    contract's "or completes" branch, and the evidence now recorded under
    finding 4.
  - **The stalled transfer ended on the software deadline, not on the
    hardware.** With SCL held low and `to_ms = 100`, the driver reported
    failure about 110-120 ms later (the log's 589 ms and 709 ms bracket it):
    the S3's SCL-stretch timeout never fired for an SCL held low before
    START. No time was left to probe, so it classified as `TIMEOUT` straight
    away. Inside the 150 ms the check allows, by about 30 ms -- `to_ms` plus
    two ticks, as the deadline arithmetic predicts.
  - **The bus worked again after the stall without `recover()`**, through the
    reset-before-next-transfer path.
  - **Absent-address transfers fail in under a millisecond each** (three
    classified failures share one log millisecond), and the driver's
    unbounded busy-wait after a NACK (finding 5) did not spin.
  - **A 1 ms deadline completed** a transfer, which the driver's own tick
    truncation would have failed (finding 2).
- **The driver logs its own errors** (`i2c.master: ... nack detected`,
  `I2C hardware timeout detected`) whenever a transfer fails. Phase 3's
  sensor polling of an absent sensor would repeat them every cycle; whether to
  quieten the `i2c.master` tag is that phase's decision.
- **A new third-party footprint:** `esp_console` links linenoise (BSD-2-Clause)
  and argtable3 (BSD-3-Clause, with the BSD/Tcl-style notices of the code it
  bundles) into the firmware. Both ship inside ESP-IDF; both are recorded in
  `THIRD_PARTY_NOTICES.md`, whose binary-redistribution notice obligation they
  add to.
- **Every transfer failure costs one extra probe**, bounded by the caller's
  own deadline.
- **The console's terminal probe shows in some terminals.** linenoise sends
  `ESC[5n` to detect a capable terminal and waits up to 500 ms for an answer;
  a terminal that does not answer (YAT, the one this project's logs come
  from) shows a stray `[5n`, then linenoise's "does not support escape
  sequences" notice, and the REPL runs without line editing or history.
  Commands still work -- confirmed 2026-10-13: `help`, `i2c_scan` and
  `i2c_recover` typed in YAT on the board, the scan matching the boot scan
  and a recovery of the healthy bus taking zero clocks.
- **The console's probe cost a second of boot, until it was moved.** The
  first boot logs showed about 1.0 s between the adapter's "bus up" line and
  the console's "ready" line. The scan accounts for 25 ms of it
  (`I2C_SCAN_RESULT ... scan_ms=25`, measured 2026-10-13: ~0.2 ms per
  address). The rest is linenoise's terminal probe, which
  `esp_console_new_repl_usb_serial_jtag()` runs in the *caller's* task
  (`esp_console_setup_prompt()`): 50 polls of `usleep(10 ms)`, each of which
  sleeps about 20 ms at the 10 ms tick because `vTaskDelay(1)` can wake early
  and `usleep()` then waits another tick -- 50 x 20 ms is the second. Called
  from `app_main` before the display, it delayed the first frame by that
  much. The console now starts last, after the event loop, where the second
  blocks nothing -- confirmed on the board: the first-frame decision moved
  from `now_ms=1098` to `now_ms=89`, the console reports ready about a
  second later while the full refresh is already drawing, and that refresh
  takes its usual 3.2 s.
- **`scl_wait_us` is the driver's default** (2 ms, ~3.3 ms on the S3's XTAL
  clock). Nothing on the bus is known to stretch the clock; a device that
  does will need it raised, per device. It did not fire for an SCL held low
  before START (above), so it is a limit on stretching mid-transfer, not a
  bound on how long a call can take -- the deadline is that.
