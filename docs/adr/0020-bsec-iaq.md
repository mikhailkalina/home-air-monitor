# 20. BSEC2 IAQ: Bosch's library drives the BME68x, behind a build option

Date: 2026-10-16

## Status

Accepted. Built for the board -- the firmware and the on-target test app,
both with the library -- and verified on the host, against the BME680 model,
with Bosch's own x86-64 Linux build of the library. **First run on the board
2026-10-16** (ELF `6d984c914...`, 21 minutes): BSEC ran its heater profiles
as on the host, finished its run-in and reached accuracy `low` after 197 s,
and called late twice -- 459 ms at the boot's full refresh, 490 ms at a
partial one. **Second run** the same day (ELF `a3fca4872...`, 21 minutes,
112 redraws under deliberate breathing): accuracy `high` at 955 s through 17
late calls of 0.3-1.6 s, one at every full refresh -- for this rate of
lateness, "Timing" below needs nothing yet (docs/hardware/board_notes.md,
open item "BSEC on the board").

## Context

The requirements want an index of air quality from the BME68x (§4.1, §13.3)
and accept raw gas resistance until there is one (§17.2), which is what
phases 3b and 3c show. The index is Bosch's BSEC library: a closed,
precompiled signal-processing library that drives the sensor itself and
returns IAQ with an accuracy. `docs/architecture.md` §4.2 keeps room for its
state in `persisted_state_t`, and §12 lists "BSEC unavailable for the host
architecture" as a risk.

The archive was downloaded by the owner after accepting Bosch's licence
(2026-10-16): **BSEC 2.6.1.0**, `bsec2-6-1-0_generic_release_22102024`, whose
release notes are dated 2024-07-29. What it holds decides most of what
follows:

- **Two solutions**, `bsec_IAQ` (IAQ, raw signals, LP and ULP) and
  `bsec_IAQ_Sel` (adds the BME688's gas scanner). The IAQ one is enough.
- **Libraries for many targets**, among them `bin/esp/esp32_s3/` and
  `bin/gcc/Linux/m64/` -- the latter a static archive of x86-64 ELF objects.
  So the host *can* run BSEC.
- **Configurations** for the BME680 and the BME688, each at 1.8 or 3.3 V, a
  measurement every 3 s or 300 s, and 4 or 28 days of calibration history.
- **No licence file.** The licence is the one accepted on Bosch's site. The
  headers, the configurations and the examples carry Bosch's header: "All
  Rights Reserved. Confidential", and "It is not allowed to deliver the
  source code of the Software to any third party without permission of
  Bosch Sensortec". `THIRD_PARTY_NOTICES.md` had listed the BSEC2 headers as
  BSD-3-Clause in a `drivers/bme68x/bsec` that never existed; that was wrong
  for this archive and is corrected there. **Nothing of the archive -- not a
  header, not a configuration array -- may enter this repository**, which is
  public.

And one fact about this firmware that the library makes matter: the event
loop flushes the panel on its own task, synchronously -- a partial refresh
holds it 0.3-0.6 s, a full one 3.2-3.7 s (ADR 0008, board_notes.md) -- while
BSEC asks to be called every 3 s, at most 6.25 % (187 ms) late (its
integration guide, FAQ 3.2 and 3.3.4.1).

## Decision

### Behind a build option, off by default

`CONFIG_HAC_BSEC` (in `components/hac_drivers/Kconfig`, so the test app sees
it too) on the board, `HAC_BSEC` on the host, both default off. CI and a
clone without the archive build exactly what they built before; the default
firmware image holds no BSEC symbol and no BSEC string (checked with `nm` and
`strings`). `cmake/bsec.cmake`, shared by both builds, says where the archive
is (`third_party/bsec/`, gitignored, or `HAC_BSEC_DIR`), names the one
configuration both link, and stops the configure step with what to download
when a file is missing -- not a link error later.

The firmware with BSEC is built from an overlay, `sdkconfig.bsec`, into a
directory and sdkconfig of its own (`build-bsec/`), as the calibration image
is, so no existing sdkconfig decides whether the symbol is set (CLAUDE.md:
a symbol that never reached sdkconfig compiles out silently). The test app
takes the same overlay with `-D HAC_BSEC=ON`. Bosch's configuration is
compiled where it lies in the archive (`bsec_iaq.c`), under the vendored
code's relaxed options; Bosch's headers are SYSTEM includes.

### A second BME68x source, chosen by the composition -- no port

`drivers/bme68x/bme68x_bsec_source.c` is a `sensor_source_t` beside
`bme68x_source.c`. The two share the sensor's bring-up -- start-up time,
Bosch's soft reset, chip and variant ids, the check that it sleeps, the
calibration log -- through `bme68x_common.c`, moved out of `bme68x_source.c`
unchanged; that source's tests are the check that nothing moved. The
firmware's `main.c` composes one or the other, never both: they would fight
over the sensor.

No port. There is one real implementation of "the BME68x under BSEC" and no
second to abstract over (CLAUDE.md), so architecture §12's `iaq_engine`
interface is not created. The simulator keeps replaying IAQ from CSV
(`replay_source` has an `iaq` column) in replay mode, and the forced-mode
driver in fake-i2c mode. A mode of `bme68x_source` was the alternative; it
would have put `#if` around BSEC calls in a file every build compiles, where
a file of its own is compiled only where the library is.

The source builds on the host against the x86-64 library, so with
`HAC_BSEC` its unit tests (`tests/unit/test_bme68x_bsec_source.c`) and the
`port_sensor` contract run there, over the fake bus and the BME680 model --
never in CI, which has no library. What CI does run is the mapping of BSEC's
verdicts onto `value_status_t`, a pure file free of Bosch's headers
(`bme68x_bsec_status.c`, `tests/unit/test_bme68x_bsec_status.c`).

### BSEC sets the schedule and the heater

**Low-power mode, a measurement every 3 s** (`HAC_BSEC_PERIOD_MS`). Ultra-low
power, every 300 s, would make BSEC's temperature and humidity -- which come
from the same measurement -- as stale as its IAQ; that is phase 5's choice
for a battery, not this one's on USB.

**The call is BSEC's.** `bsec_sensor_control()` says at each call whether to
measure, with which oversampling and which heater temperature and duration,
and when to call next. The source makes the call only once that time has
come, from whichever of `start()` and `read()` the sensor manager makes then,
and `ready_in_ms()` points at it; the manager is told the period as the
source's interval. Never early: on the host, a call 1 s early is answered
with `BSEC_W_SC_CALL_TIMING_VIOLATION`, no measurement, and the old next-call
time. BSEC counts the next call from the actual call, not on a fixed grid
(host, 2.6.1.0), so one late call does not make the later ones late.

What BSEC asks of this configuration, seen on the host: forced mode
throughout; its "power-on conditioning" first -- 380 C four times, 150 C
once and 200 C twice, each for 1000 ms -- and from the eighth measurement
320 C for 197 ms. Settings are applied only after the sensor is seen asleep, as the
forced-mode source does (Bosch's `bme68x_set_op_mode()` waits for sleep with
no bound). BSEC is handed temperature, humidity, pressure and -- when the
sensor marks it valid -- gas resistance, stamped with the time of the call
that started the measurement, as Bosch's own integration example does; heater
stability is logged, not required, also as Bosch's example does.
`BSEC_INPUT_HEATSOURCE`, the board's own heat at the sensor, is
`HAC_BSEC_DEVICE_HEAT_C`: an ESTIMATE of 0 until a reference thermometer
beside the device gives it a value.

BSEC's multi-instance interface, with the instance (1,296 bytes, read off the
library) in the source: no global, so tests make several sources and they do
not share what BSEC learnt.

### The configuration: `bme680_iaq_33v_3s_4d`

A BME680 (read back on the board, 2026-10-16); 3.3 V, the switched rail the
module runs from -- what its own regulator hands the chip is not measured, and
matters only to BSEC's self-heating compensation; 3 s, the rate above; four
days of history rather than 28, because BSEC's state restarts at every boot
until phase 5 keeps it, and a 28-day window would never fill. Named once, in
`cmake/bsec.cmake`, and logged by `init()`.

### BSEC's accuracy as `value_status_t`

| BSEC | value_status_t |
|---|---|
| IAQ accuracy 0, "stabilization / run-in ongoing" | `VAL_WARMUP` |
| IAQ accuracy 1, 2, 3 | `VAL_OK` |
| IAQ accuracy outside 0-3 | `VAL_ERROR` |
| no IAQ returned (no valid gas this time) | `VAL_ERROR` |
| gas not marked valid by the sensor | `VAL_ERROR` |
| gas valid, stabilization or run-in status still 0 | `VAL_WARMUP` |
| gas valid, both statuses 1 | `VAL_OK` |

Only accuracy 0 is a warm-up: the index BSEC reports then is a placeholder,
and the state ends by itself within a minute of each start (17 measurements
on the host). Accuracy 1 means BSEC has not yet trimmed its scale against the
room's air both clean and polluted, which may take hours or days and starts
again at every boot while the state is not kept. Treating 1 as a warm-up
would have the IAQ cell say "warming up" most of every day for an index that
is computed, only not yet scaled to this room. `value_status_t` has no "low
accuracy"; the accuracy is logged on every change instead, and is not on the
screen.

Temperature and humidity are BSEC's heat-compensated outputs; pressure and
gas resistance the sensor's own, passed through. **BSEC's CO2 equivalent is
not subscribed to**, so not even computed: an estimate from VOCs must never be
shown or published as CO2 (requirements §4.1, §13.3), and the source has no
CO2 capability. Its breath-VOC equivalent, in ppm, is not the VOC index
either, and is not subscribed to.

### What survives what

A second `init()` -- the sensor manager starting a failing sensor over --
takes BSEC's state from the instance before resetting it and gives it back
after, so a sensor unplugged and replugged keeps what BSEC learnt since boot.
On the host after a power cycle of the model: the accuracy is kept, BSEC
reports its run-in done at once, and it runs its conditioning profile again.
Nothing survives a reboot: `persisted_state_t` is phase 5's and NVS phase 6's,
and reaching for NVS here would run ahead of them. IAQ accuracy starts from 0
at every boot (board_notes.md, open item "BSEC's state restarts at every
boot").

### Timing: the panel holds the event loop, and that is accepted for now

The firmware's event loop flushes the panel on its own task, so a call due
during a refresh is made when the refresh ends: every full refresh (at most
one per 105 s, ADR 0019) delays one call by up to 3.7 s, and a partial
refresh delays one when it happens to start within 0.6 s of a call. BSEC's
tolerance is 187 ms, and its FAQ names timing outside it as the reason IAQ
"does not change or accuracy remains 0".

What the host shows of the library, with synthetic data over three simulated
hours: late calls are answered with `BSEC_W_SC_CALL_TIMING_VIOLATION` and
measured as usual, and the accuracy progresses the same with no late calls,
with one 0.6 s or one 3.7 s late every 35 calls, and with one 3.7 s late
every five. That is evidence about BSEC's arithmetic only. A late call also
means the heater plate cooled longer before that measurement, which changes
the gas reading itself, and no host test can model that.

So the firmware is not changed for it yet. Each late call is logged at WARN
with how late it was and how many there have been (`BSEC called late
late_ms=... late_calls=...`), and the board decides. If IAQ does not reach
accuracy 3, or flickers with the refreshes, the remedy is to take the flush
off the event loop's task -- a display task, with the loop polling the sensors
while the panel refreshes -- which is a firmware change, not a core one. It is
not done now because it changes the task EPDiy's clear runs on (ADR 0008,
"Task context") and the event loop's structure for a problem not yet seen.

## Consequences

- **The image grows by 40,912 bytes** with BSEC (582,928 against 542,016;
  Bosch's own size log gives 28,844 bytes of code for the library). RAM: the
  instance and the carried state, 1,476 bytes, in the source's static
  storage, and a 4,096-byte work buffer BSEC parses its blobs in, static too,
  used by `init()` alone. The existing `memory budget` line at boot and the
  event loop's stack low-water mark show both on the board; BSEC's calls run
  on the event loop's 8 KB stack.
- **The IAQ cell shows IAQ in a build with BSEC**, and raw gas resistance in
  one without, through the view model's existing rule (ADR 0016): IAQ
  whenever something provides it. Nothing in `core/` changed.
- **An IAQ change alone does not redraw the panel.** `update_policy` has
  thresholds for CO2, temperature and humidity and none for IAQ, as it had
  none for gas resistance; the index on the screen is as fresh as the last
  redraw those, or a status change, caused. Whether to add one -- say 25 IAQ
  points, half of one of BSEC's 50-point bands, at the cost of more partial
  refreshes -- is the owner's to decide, not this phase's. **Settled by ADR
  0021** the same day: the screen names IAQ's band, and a change of band
  redraws; a number moving within its band does not. Since ADR 0026 IAQ
  redraws when its number moves by more than 5 points, as the other readings
  do.
- **BSEC's accuracy reaches the screen since ADR 0021**: the mapping above
  stands, and the accuracy now travels with the value as `value_accuracy_t`,
  drawn as a question mark beside a number BSEC rates 1.
- **A firmware image with BSEC carries Bosch's proprietary library** and must
  not be attached to a GitHub release (THIRD_PARTY_NOTICES.md).
- **The host can run BSEC**, so architecture §12's risk is narrower than it
  read: on a 64-bit x86 Linux machine with the archive, `cmake --preset
  host-bsec`. CI cannot, and is held to the pure mapping only.
- **The forced-mode source stays**, unchanged in behaviour, for every build
  without the library; its three ESTIMATEs (`bme68x_limits.h`) still apply
  there, and a build with BSEC replaces them with BSEC's heater profile, its
  run-in and its heat compensation.
