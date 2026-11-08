# 40. A battery gauge from the voltage, ahead of phase 5's power work

Date: 2026-11-07

## Status

Accepted, at the owner's request: a percentage on the screen now, so that a real
cell can be run down and timed before phase 5's measurements and decisions.
Verified on the host; the firmware builds. Not yet seen on the board
(`docs/hardware/board_notes.md`, open item "The battery gauge on the board").

## Context

The status bar has had a place for "Battery 87 %" since phase 1, and the board has
said "Battery n/a" in it ever since: nothing fills `air_reading_t.battery_percent`.
The board has no gauge, only a divider from the battery node to GPIO14
(`board_notes.md`, "The battery path"), which the console's `vbat` has read since
phase 4e, 32 of 32 beside an idle link.

Phase 5 is to bring `port_power` with a simulator behind it, and much about it is
undecided: whether firmware can tell USB from a battery, what `Q3` does to a cell on
USB, how sleep works. None of that is needed to answer the owner's first question,
how long a cell lasts on today's firmware. A number and a line in the card's log are.

## Decision

**The estimate is a table, and the composition's.** `core/app/battery_gauge.c` turns
millivolts into a percentage through a table it is handed, linear between points.
The table (`platform/esp32_t5s3/board_battery.c`) is an ESTIMATE, a common lithium
polymer resting-voltage table; a run at the firmware's steady current gives this
cell's, since charge left is then time left.

**A median, then a step.** Every 12 s, a burst of 16 conversions, averaged. The
number comes from the median of the last five bursts, so that two sags in a minute --
a full refresh, the radio -- do not read as charge gone, and it moves only once the
estimate is two points from it, or at either end of the table, so that noise on the
curve's flat middle does not widen partial refreshes up to the status bar.

**Outside what a cell can hold, no percentage.** Above 4.4 V, or below 2.5 V, the bar
keeps saying "n/a", and the voltage stays in the log. By the schematic, USB with no
cell reads there -- `Q3` joins the node to the LDO's input while `PWR_EN` is high --
so a board on USB alone looks as it did: not "Battery 100 %". With a hysteresis of
100 mV, so that a supply resting on a limit does not redraw. Both limits are
ESTIMATES until `vbat` has read USB alone and a cell.

**On USB with a cell, it shows what the voltage says.** That reads high while a
charger pushes, and it does not mean charging: nothing tells firmware that yet. It is
accepted for now, and is phase 5's to settle.

**It redraws past 5 points.** `update_policy` gains the battery as a sixth reading
with a delta, reason `battery`, half of it counting as an offset for ADR 0028's
`settle`. Between those it is carried by whatever else redraws. Without it a still
night, eleven redraws in seven hours, would leave the number that far behind. The
number's status changing redraws as any status does.

**Its own age.** The gauge stamps its numbers with the time they are read at, because
`air_reading_apply_age()`'s limit is the sensors' 30 s. A refused burst changes
nothing, and five minutes without a good one is `VAL_ERROR`.

**A plain function and a hook, no port.** Architecture's rule asks for two real
implementations before a port, and the simulator has no battery: `board_battery.c`
reads the ADC, and an `event_loop_deps_t.battery` hook fills the reading on the
pump's task. A burst never waits: `adc_oneshot_read()` tries ESP-IDF's lock and
refuses at once. The console's `vbat` reads through the same function, since an ADC
unit has one owner.

**A line a minute.** `battery: sample state= percent= median_mv= burst_mv= refused=
took_us=`, at INFO, so that the card keeps it: on a battery there is no terminal,
since USB would change what is being measured.

## Consequences

- The firmware shows a percentage on a cell and "n/a" on USB alone, if the schematic
  is read right. Image +1.6 KB; the ADC's unit stays allocated, and with it, by
  ESP-IDF's source, the SAR ADC's power: a phase 5 question, small beside today's
  current.
- The percentage is rough: a table not of this cell, under load, through an ADC and
  1 % resistors that nothing has yet compared with a meter.
- The simulator's bar keeps saying "n/a"; `adp_power_sim` is phase 5's. The unit test
  runs a whole discharge through the gauge, and the golden frames already draw a
  percentage.

## Not done

- **MQTT.** ADR 0029 left `battery` to phase 5; the state message still has no such
  key. The card's log is what this run needs.
- **A cut-off.** The board has no over-discharge protection (`board_notes.md`, "The
  battery path"), and this does nothing about it: at 0 %, about 3.27 V, the firmware
  goes on running. A firmware cut-off means a deep sleep nothing here has tried; it
  is the owner's to ask for.
- **`charging`**, which stays false, and the open item on it stays open.
- **The diagnostics screen** shows no battery line; the log and `vbat` have the
  millivolts.
