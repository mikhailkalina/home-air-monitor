# 41. Touch, RTC and battery on the diagnostics screen

Date: 2026-11-08

## Status

Accepted, at the owner's request after reading the card's 24 days. Amends the
diagnostics screen's layout (ADR 0025, 0031, 0038): the clock moves from the
last line to the title's middle. Verified on the host; the firmware, its BSEC
variant, the test app and the slideshow build. Not yet seen on the board
(`docs/hardware/board_notes.md`, open item "Touch, RTC and battery on the
diagnostics screen").

## Context

The diagnostics screen showed the sensor sources' health -- state, failures,
restarts -- but nothing of the two other devices on the same bus, the GT911
touch controller and the PCF8563 RTC, though each has failed in its own way on
this board: the GT911 came back at its other address after a power glitch and
the firmware of the day panicked looking for it (ADR 0025, "The controller's
address"); the RTC lost 23 minutes across one power cut with its VL bit clear
(board_notes, open item "Time from the network on the board", 2026-10-28). And
since ADR 0040 the battery has a number the screen did not show beside its
voltage, which is the measured fact the percentage is estimated from.

The screen was full: thirteen lines of 32px text, the sources' section with
room for four lines of which the board uses two.

## Decision

- **The devices take the sources' free lines**, in the sources' columns: the
  name, the state, `failures N`, and one detail where a source shows its
  restarts. The section holds four lines, the sources first and the devices
  while room remains -- with the board's two sources, both devices. A third
  sensor source would push the RTC off the screen; the layout is to be
  revisited then, not before.
- **The core keeps the words, the composition the facts.** `diag_facts_t`
  gains `diag_touch_t` and `diag_rtc_t`: a state from a small enum
  (`DIAG_DEVICE_NONE` -- no line -- `STARTING`, `RUNNING`, `ASLEEP`, `MISSING`,
  `NO_TIME`), the failures since boot, and the touch controller's bus address
  or the RTC's drift. `diag_view_build()` turns them into text as it does
  everything else, so the words are unit tests (`tests/unit/test_diagnostics_view.c`).
  Missing and no time are trouble, black; asleep is a choice, grey.
- **The details**: the touch controller's address, `at 0x5D` or `at 0x14`,
  only while it answers; the RTC's drift against the network's time, `drift
  -20.8 ppm` -- a tenth of a ppm up to 99.9, whole ppm to 999, `drift -` before
  the second sync measures it. No `+` for a clock that gains: it would not fit
  the column beside 99.9.
- **On the board**: the touch controller's state comes from the adapter
  (`adp_input_health()`), which never waits for the controller's lock -- the
  touch task holds it through a read or a search -- and returns the last
  snapshot when it is taken. The RTC's comes from a small health record in
  `wall_clock.c`: the boot's verdict, then every write's -- `time_set` and
  `rtc_stop` at the console, net_time's hourly write -- a word each, read by the
  event loop without a lock. Its drift is net_time's last.
- **The battery on the last line's left**, `Battery 74 %` and then `3976 mV`:
  the status bar's number in the home screen's words (`n/a`, `ERR`), and the
  gauge's median whenever it has a voltage, a cell's or USB's. All of it
  redraws as it changes, the millivolts too -- the median moves at most bursts,
  12 s apart -- since ADR 0038's amendment the same day; this ADR first left
  the millivolts as of the drawing, as the Wi-Fi's signal was, to spare the
  redraws. The owner's verdict: the screen is up minutes at a time, and live.
- **The clock to the title's middle.** Clock, battery and recorder do not fit
  one line at their widest -- "Clock: set (--wall-time)", "Battery 100 % 4400
  mV", "Recording: no medium", over 1,000 px against 872 -- and the title's
  middle held nothing. Every word stays as it was; none is abbreviated.
- **The simulator has neither device**: its touch is a window or a script, its
  wall clock the virtual clock's. It passes none, and shows no line for them,
  rather than a state it would have to invent.

## Consequences

- A boot's diagnostics screen says where the touch controller latched, whether
  the RTC held its time through the outage before it (`no time` in black until
  the network or `time_set` writes it), and how fast the RTC drifts -- without
  a terminal.
- The touch adapter grows a 12-byte snapshot; the event loop's stack a
  `net_time_status_t` while it builds the facts (it has 3.3 KB to spare). The
  diagnostics view is about 130 bytes larger, twice over in the screen
  controller's static state.
- The screen redraws for a device's change: a new failure, a state, an address,
  a drift an hour apart -- all rare -- and for the battery's millivolts every
  12 s or so; ADR 0038's partial content redraws carry them, at once since its
  amendment.
- An RTC whose oscillator stopped with VL clear still reads `running`: the
  chip does not say so, and only the network's comparison shows it (the drift's
  sync, and its log line). The screen is no better than the chip's own report.
