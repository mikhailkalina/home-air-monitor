# 30. Telemetry ports and the reporter

Date: 2026-10-20

## Status

Accepted. Phase 4b, branch `phase-4b-reporter`. Verified on the host only:
contract suites for both ports against the simulator's network and an MQTT
broker model, each catching six broken wrappers; unit tests of the publish
policy and the reporter on a fake clock; a CLI test of the simulator. The
firmware and the on-target test app build, with the reporter wired into the
event loop and no adapters to give it: nothing is sent from the board until
phase 4c brings Wi-Fi and esp-mqtt behind these ports. Nothing has been seen
in a real broker or a real Home Assistant.

## Context

ADR 0029 settled what goes over the wire, as pure functions. Phase 4b is who
sends it, when, and over what -- with the board's adapters still to come, so
the decisions are made against a model of a broker, as the drivers were made
against models of their sensors.

What the requirements ask: MQTT with Home Assistant's discovery (10.1); a
retained state and an availability topic (14.3); a publish every 30 to 60 s on
USB power (11.2); and that a Wi-Fi failure not block the measurements, nor an
MQTT failure the screen (15.1). Architecture section 5.6 sketched `port_net`
and `port_telemetry`; section 3 put the publish thresholds into
`update_policy`; section 6.1 designed `app_core`, which still does not exist.

## Decision

### Two ports

**`ports/port_net.h`, as section 5.6 sketched it**: `up()`, `down()`,
`state()`, `rssi_dbm()`. `up()` asks for the link and keeps asking -- a lost
access point is sought again by the adapter, as a Wi-Fi manager does, and the
state is `NET_CONNECTING` meanwhile, never `NET_DOWN`, which means "not asked
for". Nothing waits. In phase 4 the composition calls `up()` once and the core
only reads; phase 5's power policy will be the caller of `down()`.

**`ports/port_telemetry.h`**: `connect()` with a session -- client id,
keepalive, last will -- `disconnect()`, `publish()`, `subscribe()`,
`is_connected()`, and **`poll()`**, non-blocking, as `port_input` has.

- **Every call returns at once.** `publish()` copies the topic and the payload
  into the adapter's queue before it returns; it says `HAL_ERR_NOT_READY`
  while there is no session and `HAL_ERR_NO_MEM` when the queue is full, and
  accepts nothing then. A connection, an acknowledgement, a message arriving
  happen later, on the adapter's task, and the adapter wakes the event loop
  (`event_loop_post_network()`).
- **`poll()` is a departure from section 5.6**, whose sketch had no way in
  for what arrives. The alternative is a callback, and on the board it would
  run on the MQTT client's task: core code on a second task, or locking in a
  core designed single-threaded and run-to-completion (section 6.1). With
  `poll()` the adapter queues and wakes the loop, and the core takes what
  arrived when it runs -- the shape `port_input` has had since phase 3g, so
  everything that arrives from outside comes in the same way.
- **The port neither reconnects nor keeps messages for a later session.**
  The core decides when to try again, and sees every new session start --
  which it must, since each one begins with "online" and the discovery
  configs. esp-mqtt does both by default (its automatic reconnection, its
  outbox kept across reconnects), so phase 4c's adapter turns the first off.
  A message accepted and not yet delivered when a session drops "may be lost":
  the port allows either, and the reporter does not depend on it.
- **The rules are shared**, in the header, as `port_files`' path rule is:
  what a topic, a filter and a client id may be, so every implementation
  refuses the same arguments. Bounds: a topic of 127 characters, a payload
  under 1024 bytes (the longest discovery config, ADR 0029), 256 kept of a
  received one, at least 4 subscriptions and 8 waiting messages.

### The reporter

`core/reporting/reporter.c`, a state machine between the two ports --
offline, connecting, online:

- **When to connect**: whenever the network is up and there is no session. At
  once after a session that worked drops; after a failed attempt 5 s, doubling
  to 120 s (`REPORT_BACKOFF_*`, the sensor manager's numbers and reasoning);
  an attempt not connected after 10 s is given up (`REPORT_CONNECT_TIMEOUT_MS`,
  an estimate). A session that ends before it got as far as "online", with
  the network up, counts as a failed attempt, so an adapter that accepts
  sessions and refuses everything in them is tried less and less often. The
  network going is not a failure: the next attempt is when it is back.
- **Every session starts the same**: the subscription to
  `<discovery prefix>/status`, then "online" on the status topic, then all
  nine discovery configs -- the empty ones too, which remove what the build
  does not provide -- then the state, at once.
- **The last will** is "offline", retained, on the status topic; the keepalive
  60 s (`REPORT_KEEPALIVE_S`). A device that vanishes is offline in Home
  Assistant 90 s later, before `expire_after`'s three heartbeats.
- **Home Assistant's birth** -- "online" on its status topic -- makes the nine
  configs due again. Only a live one: a birth the broker delivers from its
  store when the subscription is made (MQTT 3.1.1 marks those retained) says
  Home Assistant was up before this session, which the session's own configs
  already cover. The state is not sent again: it is retained, and the new
  entities read it.
- **Nothing published while offline is kept for later.** The card keeps the
  history (ADR 0024), and the next session starts with the newest state; a
  queue of old states would replay into Home Assistant's history as if they
  were new, stamped with their arrival. Within a session a refused message
  (the adapter's queue full) is tried again after a second
  (`REPORT_RETRY_MS`), and what follows waits for it; the state then carries
  the newest reading, not the one refused.
- **The network reaches the message, never the screen.** The reporter copies
  the reading the screen has and sets the copy's `rssi` from `port_net`; the
  screen's reading never gets the link. So an outage redraws nothing -- which
  the CLI test holds -- and the status bar's "Wi-Fi"/"offline" still says
  "offline" on the board: see "Not decided" below.

### When the state goes out: a module of its own

`core/reporting/publish_policy.c`: at once in a new session; on a change of
any value's status, or of IAQ's accuracy (any step of it: the message carries
the word, where the screen only marks low); when a value has moved by strictly
more than its delta since the last message; at least once per heartbeat, 60 s
(requirements 11.2); at most once per sensor cycle, as the display. Gas
resistance has no delta: a diagnostic in Home Assistant, drifting all day, it
rides with the others.

**A departure from section 3**, which put "display refresh and publish
thresholds" in `update_policy`. The two share a shape and, today, their
deltas -- not their reasons. The display's rules are about panel wear: a
budget of partial refreshes, the panel's minimum interval between full ones,
a request from a person, an offset left on the glass. None of that applies to
a message, and a heartbeat would be wrong on the panel (ADR 0023 removed it).
In one module, every change to one would have to be argued against the other.

### The thresholds

Estimates, in one header (`core/reporting/report_limits.h`), sized by
replaying the owner's recordings of 2026-10-18 and 19 -- the card's
`data/`, local only, never in this repository -- through the simulator:
`--recording` with each day's two files, `--telemetry stdout`, `--duration
86400`. Each state message's values were then held against every recorded row
-- the SCD41's every 5 s, BSEC's every 3 s -- as what Home Assistant showed at
that moment.

The first finding decides most of it: **every message moves the baseline,
the heartbeat's included**, so a value drifting slower than a delta a minute
never passes one. `normal_day.csv` makes no delta message in 24 hours; on the
owner's days nearly every one is IAQ's, whose index moves fastest.

| deltas: CO2, T, RH, p, IAQ | messages, 18th / 19th | over 1,440 | HA's IAQ more than 5 from the air | more than 10 | furthest |
|---|---|---|---|---|---|
| none (heartbeat only) | 1,441 / 1,470 | 0 % / 2 % | 3.0 % / 3.0 % | 0.93 % / 0.97 % | 33 / 29 |
| **50, 0.2, 1, 1, 5 -- the display's** | **1,554 / 1,597** | **8 % / 11 %** | **0.82 % / 0.78 %** | **0.017 % / 0.017 %** | **14.5 / 13.9** |
| 25, 0.1, 0.5, 0.5, 2.5 | 1,881 / 1,961 | 31 % / 36 % | 0.10 % / 0.11 % | 0.017 % / 0.003 % | 14.5 / 10.4 |
| 50, 0.2, 1, 1, 10 | 1,466 / 1,498 | 2 % / 4 % | 2.3 % / 2.8 % | 0.10 % / 0.10 % | 17.6 / 17.2 |
| 100, 0.5, 2, 2, 10 | 1,461 / 1,494 | 1 % / 4 % | 2.3 % / 2.8 % | 0.10 % / 0.09 % | 17.6 / 17.2 |

(The heartbeat-only row is above 1,440 on the 19th by its status messages: 72
changes of IAQ's accuracy between medium and high.)

**The display's deltas**: what the glass redraws for reaches Home Assistant
as soon, for 8 % and 11 % more messages than the heartbeat alone. Of the
messages that were not heartbeats: IAQ 226 and 238, status 2 and 72, humidity
7 on the 18th, temperature 8 and CO2 1 on the 19th. Halving the deltas takes
less than a percentage point off IAQ's distance for three times the extra
messages. The other readings are within their deltas in Home Assistant over
99.9 % of the rows in every variant; CO2's furthest, 195 ppm, came between
two of the simulator's 5 s polls, which no delta reaches.

To replace: the board's own days with its broker, and how Home Assistant's
database grows with them.

### The event loops, and `app_core`

`apps/firmware_esp32/main/event_loop.c` and `apps/simulator/main.c` run the
reporter after each poll, as the recorder's watch, and wake at the earliest
of the sensor manager's, `update_policy`'s and the reporter's deadlines (and
the screen controller's). On the board the network and MQTT adapters wake the
loop through `event_loop_post_network()`; in the simulator the loop looks
when the simulated network changes, or the broker model's client has news.

**Is a third module driven identically by two loops the time for
`app_core` (section 6.1)? Not yet.** What the two loops repeat is a dozen
lines -- call, log the note, take the minimum -- and the shape `app_core` is
for is not settled by them. It pays off in phase 5: deep sleep is a cold boot,
and `app_core_prepare_sleep()` has to serialise the sensor manager, both
policies, the screen controller and the reporter into `persisted_state_t` and
restore them, with `allow_sleep` decided across all of them. Writing it now
would fix its interface before its hardest requirement is known. The cost
meanwhile: the firmware's loop has no host test of its own, and two loops can
drift; the simulator's CLI test holds one of them.

### The host: a network and a broker

- **`platform/host/adp_net_sim.c`**: the access point there or not, an
  association of 3 s (an estimate) after `up()` or after it is back, a fixed
  RSSI. `--net-script "<s>:down;<s>:up"`, in the style of `--taps`. A
  listener hears each change at its own time.
- **`platform/host/adp_telemetry_loopback.c`**: the MQTT counterpart of the
  I2C device models. A broker as MQTT 3.1.1 has it -- retained messages, `+`
  and `#`, sessions by client id starting clean, a second connection with an
  id taking the session over, the last will one and a half keepalives after
  the network went or at once on a takeover, deliveries from the store marked
  retained -- and a client of it behind `port_telemetry`, whose queue fills
  while the broker is stalled. Scriptable: attempts refused at once
  (`HAL_ERR_IO`) or never answered, and a stall. The simulator's network
  drops the client's session through the listener.
- **`--telemetry stdout|loopback`**: both the broker model; `stdout` prints
  every message it takes, `loopback` its sessions and wills, and a summary.
  Not a separate sink: one that cannot receive would fail half the port's
  contract, and a second model would be a second thing to keep right.
  `--device-id` names the device (`simulator` by default); the prefix is
  requirements 10.1's `homeair`.

## Consequences

**Verified on the host.**

- *Contracts*: `port_net` against the simulated network (6 broken wrappers
  caught: an `up()` that waits, a signal while down, a state stuck at up, no
  reconnection, a `down()` ignored, a link brought up unasked);
  `port_telemetry` against the loopback client (6 caught: a payload not copied,
  success without a session, a session that never ends, retain dropped, a
  `publish()` that waits, a full queue reported as success). Both run a
  second time with the runner unable to cut the network or stall the broker,
  as the board's will be, and report those checks as not exercised.
- *Unit tests*: the publish policy at every delta's boundary, every status,
  the heartbeat to the millisecond, the cycle, a restarted clock; the reporter
  on a fake clock -- the first session's messages in order, the heartbeat, an
  outage (nothing published, the will at 90 s, the next session with
  everything and the newest state), a session dropped with the network up (the
  old will at the takeover, before the new "online"), Home Assistant's birth
  live and retained, a refused publish and the newest reading after it,
  refused and unanswered attempts and the backoff's ceiling, and a day: 1,440
  messages with nothing moving, 1,444 with an airing, 17,280 with a value past
  its delta every cycle; the broker model's own matching, takeover, will
  timing, refusals and full queues.
- *The CLI test* (`tests/cli/simulator_telemetry.cmake`): `normal_day.csv`
  for 24 hours, 1,440 state messages never more than 60 s apart; three runs
  of 30 minutes -- without telemetry, with it, with a 10-minute outage -- make
  the same redraw decisions and byte-identical recordings, and the outage's
  broker log shows the drop, the will, silence, and the next session's
  "online", 9 configs and state. Seen failing with the network's state let
  into the screen's reading.
- ASan and UBSan clean.

**The board** builds with the event loop's third deadline and no reporter
(`.reporter = NULL`). The reporting code is linked: 6.8 KB of flash, no RAM; a
`reporter_t` is 2,376 bytes, static when 4c composes one.

**What phase 4c inherits**:

- An MQTT adapter over esp-mqtt, from what its header in ESP-IDF 5.5 says
  (`components/mqtt/esp-mqtt/include/mqtt_client.h`, read, nothing tried):
  automatic reconnection off (`network.disable_auto_reconnect`); a bounded
  outbox (`outbox.limit`, bytes), whose "-2, full outbox" is
  `HAL_ERR_NO_MEM`; a publish that only enqueues --
  `esp_mqtt_client_enqueue()`, since `esp_mqtt_client_publish()` "sends the
  publish message immediately in the user task's context" and "might block for
  several seconds". Two traps there: `esp_mqtt_client_enqueue()` queues QoS 1
  messages with no connection at all, so the adapter answers
  `HAL_ERR_NOT_READY` itself; and a length of 0 means "computed from the
  payload string", so an empty message -- a discovery config that removes an
  entity -- must go as `data == NULL`. Its events become the port's state and
  inbox, and an `event_loop_post_network()`.
- A Wi-Fi adapter behind `port_net`, waking the loop on every change, and the
  device id from its MAC.
- Both contracts in the test app: `port_net` with `cut` and `restore` NULL,
  `port_telemetry` with a broker on the network and `stall` NULL.
- The event loop's stack with a session running: `reporter_step()` builds
  configs through the JSON writer on that stack.

**Not decided here**:

- *The status bar.* The screen shows "Wi-Fi" or "offline" from the reading's
  `net_connected`, which nothing sets: on the board it will read "offline"
  while MQTT works. Feeding it the link costs two status redraws per outage,
  and Wi-Fi that flaps would flap the panel; it wants a rule of its own --
  a hysteresis, or the session rather than the link -- when phase 4c shows how
  the link behaves.
- *A real broker from the simulator* (architecture section 10: "the simulator
  appears in Home Assistant"). A host adapter over libmosquitto is a new
  dependency -- dual-licensed EPL-2.0 and EDL-1.0, as far as known here, not
  checked -- which needs its licence discussed before it is added; the
  board's adapter may get there first.
- *A clean "offline"* before a planned stop -- phase 5's deep sleep -- is the
  reporter's to add then: `disconnect()` publishes no will.
- *`scenarios/wifi_flapping.csv`*, a link profile with RSSI, is still not
  read: `--net-script` covers outages, and the RSSI is fixed.
