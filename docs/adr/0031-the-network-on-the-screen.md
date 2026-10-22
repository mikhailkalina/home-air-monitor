# 31. The network on the screen

Date: 2026-10-21

## Status

Accepted. Phase 4d, branch `phase-4d-network-on-screen`. Verified on the
host: unit tests of the hold to the millisecond and over flapping links, of
the view models, of the reporter's link and status, and of `update_policy`'s
trigger; the `port_net` contract with its new call, nine broken wrappers
caught; both layout tests by pixels; golden frames looked at before they were
adopted; two CLI tests -- an outage, and a link flapping every 10 s for half
an hour -- all under ASan and UBSan, and against a real mosquitto. The
firmware, its BSEC variant and the test app build. Nothing of it can be seen
on the board until its Wi-Fi and MQTT adapters land; what changes there now is
that the status bar no longer says "offline" (below, "On the board").

**Amended 2026-10-22 by ADR 0034**: the diagnostics screen no longer draws
the bar's word in its title line, nor compares the held link -- the MQTT line
says the same; its network lines are all as of the drawing.

Answers ADR 0030's "Not decided here: the status bar", and departs from its
"the network reaches the message, never the screen".

## Context

The status bar has shown "Wi-Fi" or "offline" since phase 1, from the
reading's `net_connected`, which nothing on the screen's side ever set: ADR
0030 kept the network out of the screen's reading on purpose, so that an
outage redrew nothing, and left the bar for a rule of its own -- "a
hysteresis, or the session rather than the link". On the board the bar says
"offline" whatever the network does.

Three things decide that rule:

- **A flapping link must not spend the panel's budget.** Every change on the
  glass is a partial refresh, 20 of them a full one (ADR 0019). IAQ trembling
  on 50 made 96 % of the board's redraws for days before ADR 0026 gave it a
  delta. A Wi-Fi link that reassociates, a broker that restarts, a reporter
  whose attempts are refused, change the link's state every few seconds on a
  bad day.
- **"Connected / Offline" cannot say what is wrong.** Requirements 9.2's
  example has two words; the device has two ways of being offline -- no
  Wi-Fi, or the Wi-Fi without a broker -- which a person mends in different
  places, and a third state with no telemetry at all.
- **`update_policy` redraws on `telemetry_connected`**, which no screen draws
  (board_notes.md's open item): a status redraw with nothing to show, which
  the event loop's empty-diff skip then throws away every tick.

The diagnostics screen (ADR 0025) has nothing on the network either: neither
the link nor the session, nor how much has been sent.

## Decision

### What the bar says

| the link | the bar | ink |
|---|---|---|
| an MQTT session -- the readings published | `online` | black |
| the Wi-Fi up, no session | `no broker` | the stale grey |
| no Wi-Fi: connecting, lost, failed | `no Wi-Fi` | the stale grey |
| no telemetry in the build or its configuration | nothing | |

`online` is Home Assistant's own word for the device being there -- its
availability topic carries "online" and "offline" -- so the bar says what
Home Assistant sees. The two ways of being offline keep the grey "offline"
had: a weaker claim, as a stale number's, on a screen whose job is the air.
Without telemetry the bar says nothing, as a field never says nothing: there
is no link to describe.

**The word has a place of its own**, as wide as the widest word, at the
right margin; the battery is laid out left of it. One word giving way to
another moves nothing else -- moving text leaves its old pixels driven, and
the panel keeps them (ADR 0021). Without telemetry there is no place, and
the battery goes to the margin: that is the build's, not something that
changes at run time.

### A change shows once it has held a minute

`core/app/link_hold.c` turns the link as it is now into what the screen
shows: **a state the link has stayed in for 60 s without a break**
(`LINK_HOLD_DEFAULT_MS`, an estimate). A change shorter than that never
reaches the glass; the changes that do are at least a minute apart, so a link
costs **at most one redraw a minute, however it behaves**, and nothing at all
when it flaps faster than that.

- **The minute is an estimate** at its definition: long enough for what is
  over by itself -- a Wi-Fi reassociation (seconds), a broker restarting (a
  session again after 15 s with Home Assistant's, ADR 0030's amendment), the
  reporter's first attempts after a drop (at once, 5 s, 10 s later) -- and
  short enough that the bar says "no broker" about when Home Assistant says
  the device is unavailable: its last will goes out 30 to 90 s after the
  device fell silent. To replace: the board's own drops and reconnections,
  from the card's log.
- **Both ways.** Good news waits as long as bad: a hold for one direction
  only would let a link down for 70 s and up for 5, over and over, cost two
  redraws a cycle.
- **The first state too.** Until the link has held a state since start the
  screen shows `LINK_UNKNOWN`: the word's place, and no word -- not "no
  Wi-Fi" for the seconds the association takes and then for a minute after it
  is done. The first word appears with a status redraw a minute after boot.
- **No telemetry is shown at once**: it is a configuration, and does not
  flap.

**Not in `update_policy`.** A hold there -- the link raw in the reading, a
redraw once it has differed from the glass for a minute -- would draw the
raw link whenever anything else redrew: a CO2 redraw during a two-second blip
would put "no Wi-Fi" on the glass, and leave it there until the hold redrew it
back. Held first, the screen only ever draws a state that has lasted a
minute, `update_policy` treats it as one more status -- a change redraws at
once, under the cycle, the budget and the panel's rules -- and the
diagnostics screen draws the same state.

**Not the session alone**, which ADR 0030 weighed: it flaps with the broker
as the link does with the access point, and it cannot tell "no Wi-Fi" from
"no broker".

### In the reading: `link`, held

`air_reading_t.telemetry_connected` is gone; `air_reading_t.link`, a
`link_status_t`, is the held state: `LINK_OFF` (the zero value),
`LINK_UNKNOWN`, `LINK_NO_WIFI`, `LINK_NO_BROKER`, `LINK_ONLINE`. Both event
loops feed the hold after the reporter's step with `reporter_link()` -- the
network's state from `port_net`, and whether the reporter has a session --
put `link_hold_shown()` into the screen's reading, and wake for the hold's
end among the other deadlines. The simulator prints each change as a `[link]`
line; the firmware logs `link on the screen shown=...`.

`net_connected` and `wifi_rssi_dbm` stay what ADR 0030 made them: the state
message's, set by the reporter in its own copy, drawn by nothing.
`update_policy`'s status trigger now fires on `link`, which the bar draws,
and no longer on `net_connected` or `telemetry_connected`, which nothing
draws -- the open item's answer for the link. `charging` stays in the trigger
and in the open item until phase 5's battery adapter draws it.

### The diagnostics screen: the network's two lines

```
Diagnostics                                    online   tap to return
...
Wi-Fi  up      sim-ap        -61 dBm                    192.0.2.10
MQTT   online  sent 1537     refused 0                  last 14:07
```

- **Wi-Fi**: the link's state -- `up`, `joining` (connecting: "connecting"
  does not fit the column), `failed`, `off` -- the network's name, the signal
  and the device's address; the last two only while up.
- **MQTT**: the session's state, `online` or `offline` (an attempt under way
  is still no session); the state messages sent since boot -- exact to 9,999,
  then in thousands, `sent 12k`, to `sent 999k+`; the messages the adapter
  refused, `999+` at most, as the other counts; and the local time of the
  last state message, `last 14:07`, dated from an earlier day as the footer's
  "updated" is, `last --:--` without a wall clock, `last -` before the first.
- Without telemetry both lines say `off`. Trouble is black, as on the rest of
  the screen: the link not up, no session, a refusal.

**`port_net` gains `info()`**, the network's name and the device's address,
because only the adapter knows them: the SSID as configured -- also while the
link is not up, so a wrong name can be seen -- up to 32 octets of any value,
terminated; and the IPv4 address while `NET_UP`, 0.0.0.0 otherwise. The
contract checks both (terminated, the same name up and down, an address only
while up, never waiting), with three more broken wrappers. The screen shows
octets outside printable ASCII as `?` (the fonts have nothing else) and
shortens the name to its column. IPv4 only: a home network's address is.

**What is drawn as it is, and what redraws.** The signal, the counts, the
time of the last message change in ordinary operation, every minute at
least, and the states with every attempt; redrawing for them would spend a
partial refresh a minute while someone reads the screen. So they are drawn as
they are when the screen is drawn and redraw nothing by themselves; what
redraws the network's lines is the held link and the network's name
(`diag_view_equal()`). A flapping link costs this screen nothing either.

**The held link is drawn on it too** (*no longer since ADR 0034*: neither
drawn nor compared): the status bar's own word, beside the
hint in the title line -- black when it says something is wrong, as the rest
of the screen -- so that what redraws the screen is always something it
draws. The first version compared the held link and did not draw it: the
owner, watching the window with the diagnostics screen up when a stopped
broker had lasted a minute, saw the redraw due come out an empty diff, which
the loops skip without committing -- so it was due again at the next step,
every second in the simulator until the screen timed out, and on the board
each skip would have lengthened the event loop's backoff, which holds a
tap's redraw too (board_notes.md's open item on `charging`). The rule the
open item on `telemetry_connected` set -- a screen redraws only for what it
draws -- applies to this screen as to the home one, and a test now holds it:
every change of the held link changes the diagnostics screen's picture.

**The room for them** came from the title: the screen now holds thirteen
lines of 32px text -- four sources at most, six readings, the network's two
and the clock's -- and "Diagnostics" is 32px, as its hint always was. The
rules sit closer to the text; the layout test holds every column at its
widest by pixels, including a network name of 32 octets.

## Consequences

**Verified on the host.**

- *The hold*: the first state held, a change at the hold to the millisecond,
  one a millisecond short never shown, a link flipping every 4, 30 and 59 s
  for an hour shown not once, every 61 s at most once a minute, a restarted
  clock starting the hold over, no telemetry at once.
- *The CLI test of an outage* (`tests/cli/simulator_telemetry.cmake`): the
  recordings of three runs -- no telemetry, telemetry, a ten-minute outage --
  byte for byte the same; the link on the screen `unknown` at 0, `online` at
  63 s, and in the outage `no-wifi` at 660 s and `online` at 1,263 s -- each
  a minute after the change, each on the glass within a sensor cycle; the
  same redraws with and without the outage until it had lasted a minute. The
  part of ADR 0030's test that held all redraws the same is replaced: it is
  what this ADR changes.
- *The CLI test of a flapping link* (`tests/cli/simulator_link_flapping.cmake`,
  half an hour of `co2_spike_meeting.csv`): the access point gone 3 s in every
  10 s from 120 s on -- 169 sessions -- left the redraws the same as the
  steady run's, decision for decision, 172 of them, 9 full, and `online` on
  the screen throughout. Gone 90 s and back 90 s, every change reached the
  glass a minute late, never two within a minute: 20 changes, 177 redraws
  against 172, 9 full refreshes against 9. Seen failing with a hold of 2 s.
- *Layout*: every link word beside every battery text, and a change of word
  changing no pixel outside the word's place (seen failing with the place
  sized to the word); every network word and the widest texts in each of the
  network's columns (seen failing with the count column 12 px further left,
  and 18 px further right); every change of the held link changing the
  diagnostics screen's picture (seen failing with the title's word left out).
- *The diagnostics screen up while the held link changes*
  (`simulator_link_flapping`): one redraw at the change, no empty diff in any
  of the test's runs (seen failing with the title's word left out: 31 skipped
  redraws, one a second).
- The simulator's network (`adp_net_sim.c`) gives a name, `sim-ap`, and an
  address from the block RFC 5737 sets aside for documentation, `192.0.2.10`;
  its script holds 1,024 changes, enough for half an hour of flapping.

**On the board**, until its adapters land: no reporter, so `LINK_OFF` -- the
bar says nothing where it said "offline", and the battery moves to the right
margin; the diagnostics screen's network lines say `off`, and its title is
smaller. The firmware's event loop runs the hold -- a `link_hold_t` of 32
bytes in `main.c`, a `reporter_status_t` of 80 in the loop's state, both
static -- and nothing else changes on the board.

**What the board's adapters inherit**: `info()` in the Wi-Fi adapter -- the
SSID from its configuration, the address from its network interface while
up -- and the `port_net` contract in the test app with it. The board's open
item "The network on the screen (phase 4d)" lists what to see then.

**Seen in the simulator's window, 2026-10-21** (the owner, against the local
mosquitto, `--time-scale 1`): with `--net-script "120:down;300:up"`, the
session lost at 120 s, the will 90 s after the last message, `no-wifi` on the
screen at 180 s, session 2 at 303 s, `online` at 363 s; with the broker
stopped by hand, `no-broker` a minute after the drop and `online` a minute
after the session came back -- 85 s after the broker did, the reporter's
backoff with a 10 s give-up on each refused attempt (ADR 0030's amendment
names the port change that would shorten it). That run found the empty diff
above.

**Costs.** An outage longer than a minute costs two redraws, its start and
its end, a minute late each; a shorter one costs nothing; the bar may say
"online" for a minute after the link went, and "no Wi-Fi" for a minute after
it came back. A boot costs one status redraw a minute in, for the first word.
The diagnostics screen's counts and times are as old as the screen's last
redraw, at most the minute it stays up.

**Not decided here**: the signal on the bar (it changes every read); IPv6;
whether the minute suits the board's own Wi-Fi, which its first days with
the adapter show.
