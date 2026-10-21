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

**Amended 2026-10-20, phase 4c** (branch `phase-4c-simulator-mqtt`; "Amendment
(phase 4c): the simulator on a real broker", at the end): the simulator
reaches a real broker over libmosquitto, verified against mosquitto on the
host and in CI, and seen in a Home Assistant 2026.10.2 on 2026-10-21 (a
container beside it; "Seen in a Home Assistant", at the end). The board's Wi-Fi and
esp-mqtt adapters, which the text below gives to phase 4c, come in the
sub-phase after it.

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
  Assistant 90 s later, before `expire_after`'s three heartbeats. (*Corrected by
  the amendment*: 90 s after the last packet the broker heard, which a real
  broker counts from -- 30 to 90 s after the device vanished; 34 s seen.)
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
  the network went (*corrected by the amendment*: after the last packet the
  broker heard, with the client's pings) or at once on a takeover, deliveries
  from the store marked
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
  outage (nothing published, the will at 90 s -- 90 s after the last message
  since the amendment -- the next session with
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

**What the board's adapters inherit** (phase 4c's when this was written; the
next sub-phase's since the amendment below):

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
  device id from its MAC, in the form the amendment fixes:
  `mqtt_device_id_from_mac()`, `hac-` and twelve lowercase hex digits.
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
  board's adapter may get there first. **Decided by the amendment below**:
  the licence checked, used under EDL-1.0, and the simulator there first.
- *A clean "offline"* before a planned stop -- phase 5's deep sleep -- is the
  reporter's to add then: `disconnect()` publishes no will.
- *`scenarios/wifi_flapping.csv`*, a link profile with RSSI, is still not
  read: `--net-script` covers outages, and the RSSI is fixed.

## Amendment (phase 4c): the simulator on a real broker

Date: 2026-10-20. Branch `phase-4c-simulator-mqtt`.

Architecture section 10's milestone for phase 4 is the simulator appearing in
Home Assistant as a device, before the board sends anything. Phase 4b left it
open (above, "Not decided here"): the reporter worked against a model of a
broker only. This amendment adds the host's way to a real one. Nothing in
`core/` changes for it but a pure function for the board's device id
(below); the ports are as 4b wrote them, and the contract that holds them is
unchanged too -- it runs against one more implementation.

### `platform/host/adp_telemetry_mqtt.c`, over libmosquitto

Built only when libmosquitto 2.x is found (`SIM_MQTT`, on by default;
`hac_platform_host_mqtt`, which carries `HAC_SIM_MQTT`), as the SDL window is
built only with SDL2: without it the simulator has no `--broker` and says so,
and nothing else changes.

**One libmosquitto client per session, each on a network thread of its own
running libmosquitto's loop**, `mosquitto_loop_forever()`. Three promises of
the port are things libmosquitto does not do by itself, and this is how each
is kept:

- *Nothing waits.* `connect()` makes the client, starts the thread and
  returns; the name lookup and the TCP handshake (`mosquitto_connect()`,
  blocking) happen on that thread, so a host name costs the caller nothing
  and a broker that moves is followed. `publish()` hands libmosquitto the
  message, which copies it; the thread sends it.
- *The port never reconnects.* libmosquitto's loop connects again by itself
  after a drop. Here `on_disconnect` with an error calls
  `mosquitto_disconnect()`, which is what makes the loop return (read in the
  2.0.11, 2.0.18 and 2.0.22 sources: the first checks the client's state, the
  others a `request_disconnect` flag, and the call sets both), and the
  session is over.
- *Nothing is kept for a later session.* libmosquitto resends unacknowledged
  QoS 1 messages when it reconnects. A new client per session has nothing to
  resend.

Why not `mosquitto_loop_start()`, "the threaded interface": its thread does
the same loop, but `mosquitto_connect_async()` resolves the name and opens
the socket in the caller's thread, and in 2.0.x a loop thread that ends by
itself marks itself not running before it returns, so nothing can join it
afterwards -- with a client per session, a thread leaked per drop. The
adapter's threads are its own, joined when they end.

What arrives -- the CONNACK, a message, the end of a session -- is put into
the adapter's state and inbox under a lock by libmosquitto's callbacks, and
taken by the caller's next call (`is_connected()`, `poll()`); the waker is
called after each, on the network thread. Cancellation is held off wherever
that lock is held. Bounds: 32 messages awaiting acknowledgement before
`HAL_ERR_NO_MEM` (a session starts with 11 at once), 16 kept for `poll()`, 8
subscriptions, 8 clients at once -- the session, and earlier ones whose
threads are still ending; past that `connect()` says `HAL_ERR_IO` at once.

**How a session ends, and what the broker makes of it**:

- `disconnect()` sends DISCONNECT, so the broker drops the will. The next
  session's thread waits, before its CONNECT, until that DISCONNECT has gone
  out (at most 1 s, `ADP_TELEMETRY_MQTT_CLOSE_WAIT_MS`): a CONNECT the broker
  met first would take the old session over and publish its will. Seen
  doing exactly that before the wait existed, at the end of the contract,
  where `disconnect()` was followed at once by the adapter's destruction.
- The network taken away (`adp_telemetry_mqtt_set_link()`, which the
  simulated network's listener calls, so `--net-script` works over a real
  broker): the session goes **silent** -- its thread is cancelled where it
  waits on the socket, and the socket left open, unread and unwritten, as a
  board's is when Wi-Fi goes. The broker drops it one and a half keepalives
  after the last packet it heard and publishes the will (seen in the broker's
  log: "has exceeded timeout", 15 s after the cut with the contract's 10 s
  keepalive, which publishes just before it cuts); when the
  network comes back the old socket is closed, and a broker that has not yet
  dropped the session publishes the will then.
- The broker going -- a restart, a refusal: the session is over, and the
  adapter logs why (`mqtt: no connection to the broker`, `the broker refused
  the session ... why=Connection Refused: not authorised.`, `the session was
  lost`), each reason once until a session works again, so that a reporter
  retrying an absent broker does not fill the log.
- `adp_telemetry_mqtt_destroy()` -- the end of the simulator's run: every
  socket closed with no DISCONNECT, so the broker publishes the wills, as
  when the board loses its power. A DISCONNECT already asked for goes out
  first. A process killed outright gets the same from the kernel closing the
  socket.

### The simulator

- **`--broker mqtt://<host>[:<port>]`**, plain MQTT -- `mqtts://` is refused:
  the simulator talks to a broker on a network it trusts, which is how Home
  Assistant's Mosquitto add-on listens by default. `--broker` and
  `--telemetry` each choose a broker and are refused together.
- **`--mqtt-user <name>`, and the password from `HAC_MQTT_PASSWORD`**, never
  from the command line, where every process listing shows it. A URL with a
  user or a password in it is refused, and not printed. The password is
  printed nowhere -- the CLI test looks for it in every output -- and the
  client's copy of it is zeroed when the client is destroyed. A password without a user is not
  sent (MQTT 3.1.1, 3.1.2.9), and the simulator says so.
- **Real time.** The broker's keepalive, its will, Home Assistant's
  `expire_after` and the reporter's timeouts are only meaningful together in
  real time, so with `--broker` the loop follows real time headless as well,
  as the window does -- looking every 20 ms, woken early by the client --
  and `--time-scale` must be 1 to 60 (`SIM_BROKER_TIME_SCALE_MIN`, `_MAX`,
  choices): below 1 the heartbeat comes further apart than three of them
  `expire_after` allows; above 60 the reporter's 10 s for a connection is
  under 167 ms of real time. Home Assistant stamps every state with its
  arrival, so a recorded day replayed at x60 is 24 minutes of history there;
  at x1 it is a day.
- **A device id of its own.** The simulator is `simulator` unless
  `--device-id` says otherwise, and in front of a real broker it refuses an
  id of the board's form: `hac-` and twelve hex digits, either case. That form
  is decided here, ahead of the board's Wi-Fi adapter, so that the refusal
  has something to refuse: `mqtt_device_id_from_mac()` and
  `mqtt_device_id_is_from_mac()` in `core/reporting/topics.h`, pure, unit
  tested, for the board's composition to use. An id is a topic level, the
  MQTT client id and the stem of every entity's `unique_id`: a simulator with
  a board's would take that board's place in Home Assistant, and the board's
  entities would follow the simulator's numbers. Changing the form later
  orphans every entity Home Assistant has recorded for a board, which is why
  it is fixed now and written down.
- **The end of a run** -- `--duration`, Ctrl-C or SIGTERM (both now end the
  loop and print the summary, as `--duration` does; a second one kills), the
  window closed -- destroys the client without a DISCONNECT: the device shows
  unavailable in Home Assistant at once, through the will.
- Output is line-buffered whenever the run follows real time, so a log being
  written beside a Home Assistant can be read as it grows.

### Licence

libmosquitto is dual-licensed EPL-2.0 or EDL-1.0, as the libmosquitto1
2.0.22 package's copyright file and the upstream sources say. This project
uses it under **EDL-1.0**, the Eclipse Distribution License, which is the
BSD-3-Clause text; the EPL-2.0 alternative, a weak copyleft, is not taken, so
no copyleft question arises. It is host only: a system package, linked
dynamically into the simulator and two tests, never vendored, never in a
firmware image (THIRD_PARTY_NOTICES.md). The tests also run the mosquitto
broker, from the same source package and under the same licences, as a
separate process -- run, never linked.

### Tests

- **The `port_telemetry` contract against libmosquitto and a real broker**
  (`tests/contract/test_port_telemetry_contract.c`): a private mosquitto the
  runner starts on a free port of 127.0.0.1 from the binary found at
  configure time (`tests/support/mqtt_test_broker.c`), or the broker
  `HAC_TEST_MQTT_BROKER` names (`HAC_TEST_MQTT_USER` and `HAC_MQTT_PASSWORD`
  with it). On real time, with `cut` and `restore` as above, `stall` NULL: 45
  checks, the two of the full queue not exercised -- a real broker takes
  what it is sent. A build without libmosquitto, or one with no broker found,
  prints that the case was not exercised. 36 s.
- **`tests/cli/simulator_broker.py`**, the milestone short of Home Assistant,
  against a private broker with the simulator at x60: it appears -- "online",
  seven configs, two empty ones that remove what the scenarios do not
  provide, the state, all retained, every `unique_id` its own; the state
  keeps coming; a Home Assistant birth, live, brings all nine configs again;
  a broker restarted with nothing kept is found again and announced to; a
  SIGKILL leaves the will, retained, and so does a run that ends by
  `--duration`; a broker with a password file refuses the wrong password, and
  the log says "not authorised", and takes the right one from
  `HAC_MQTT_PASSWORD`, which no output contains; and a board's id, x3600, a
  password in the URL and a user without a broker are refused before
  anything runs. Its own MQTT 3.1.1 client watches the broker, so the broker
  is the only thing to install. Skipped -- "not exercised" -- without a broker
  or without libmosquitto. 19 s.
- **CI**: the host job installs `libmosquitto-dev` and `mosquitto`, so both
  run there; the ASan job does too, so that the client's threads and copies
  run under the sanitizers. Each test starts its own broker on a free port,
  whatever the package's own service does.
- Verified locally with libmosquitto 2.0.22 and its broker, Ubuntu 26.04's
  packages unpacked into a scratch directory (nothing could be installed on
  the development machine): both tests under the debug build, ASan and UBSan,
  and ThreadSanitizer, which no CI job runs -- all clean. The default build
  there, with no libmosquitto, passes with both cases not exercised. CI's
  runner installs its Ubuntu's version (2.0.18 on 24.04), whose loop and
  disconnect were read and are the same; the contract runner prints the
  version it ran with.

### Seen in a Home Assistant, 2026-10-21

The owner had no Home Assistant machine, so it ran beside the simulator in WSL:
Home Assistant 2026.10.2 as a container (`ghcr.io/home-assistant/home-assistant:stable`,
host network), Ubuntu's mosquitto 2.0.22 as a service (no listener configured:
127.0.0.1:1883, anonymous, `persistence true`), Home Assistant's MQTT integration
pointed at it. The simulator at x1, headless, replaying the board's recordings of
2026-10-18:

```bash
./build/host-debug/apps/simulator/simulator --headless --duration 0 \
    --broker mqtt://127.0.0.1:1883 \
    --recording <card>/data/2026-10-18_scd4x.csv --recording <card>/data/2026-10-18_bme68x.csv
```

- *The device*: "Home Air Monitor (simulator)", "simulator by home-air-monitor", nine
  entities -- CO2, humidity, IAQ, pressure and temperature as sensors; gas resistance,
  IAQ accuracy, uptime and Wi-Fi signal as diagnostics -- each with the first rows'
  values (852 ppm, 48 %, IAQ 65 and "high", 97,975 Ohm, -61 dBm). Pressure and
  temperature showed as 29.5 inHg and 73.3 F: that Home Assistant's unit system is US
  customary, and it converts what discovery declares (hPa, C) -- 999.386 hPa and 22.95 C
  exactly.
- *Home Assistant restarted* (`docker restart`): `reporter: home assistant started:
  discovery again`, the nine configs again (at 105 s), the entities kept.
- *The broker restarted* (`systemctl restart mosquitto`): `mqtt: the session was lost
  ... why=The connection was lost.`, `session lost ... why=dropped` at 181.7 s; the
  attempt at once refused (`no connection to the broker ... why=Connection refused`);
  `session up session=2` at 196.8 s, then Home Assistant's own birth as it reconnected,
  and the configs again. Home Assistant showed the entities unavailable from 10:50:06 to
  10:50:21 -- its own connection to the broker went too -- 15 s.
- *Ctrl-C*: `[sim] stopped by a signal`, the summary, the session closed without a
  DISCONNECT -- every entity unavailable at once (10:51:19), through the will.
- The run's count: 2 sessions, 1 lost, 3 attempts (1 failed); 4 state messages, 36
  discovery configs, 2 births; 42 messages published, 42 acknowledged.

**Found: 10 of those 15 s were the reporter waiting on an attempt the adapter knew
had failed.** libmosquitto's connect was refused within milliseconds, but the port has
no way to say so after `connect()` has returned: the reporter waits out its connect
timeout (`REPORT_CONNECT_TIMEOUT_MS`, 10 s), then its 5 s backoff. esp-mqtt reports a
refused connection as quickly, so the board will wait the same. A remedy is a port
change -- a way for `port_telemetry` to say that an attempt is over -- and is not made
here; it is worth weighing with the board's adapter.

**An outage**, the same set-up, `--duration 420 --net-script "120:down;300:up"`, watched
with `mosquitto_sub -F '%I %t %p'` on the status topic and in Home Assistant's activity
(local time, UTC+3):

| | the simulator, from its start at about 11:01:21 | the status topic, Home Assistant |
|---|---|---|
| session 1 | `session up session=1` at 3.0 s; the state at 3.0 s, the heartbeat at 63.0 s | "online"; available from 11:01:24 |
| the cut | `[net] connecting`, `session lost session=1 why=network` at 120.0 s | nothing |
| the will | -- | "offline" at 11:03:55, about 154 s: unavailable |
| the network back | `[net] up` at 303.0 s, `session up session=2`, the configs and the state | "online" at 11:06:24: available |
| the end | `the session ends without a DISCONNECT` at 420 s | "offline" at 11:08:21: unavailable |

**Found: the will came 34 s after the cut, not 90 s** as this amendment, the 4b text and
`report_limits.h` had it. A broker counts one and a half keepalives from the last packet it
heard (MQTT 3.1.1, 3.1.2.10) -- here the heartbeat at 63.0 s, and 63 + 90 = 153 s. Since the
heartbeat or libmosquitto's ping goes out at least once a keepalive, a device that vanishes is
offline 30 to 90 s later, never more. The statements were corrected.

**And the broker model made exact.** `adp_telemetry_loopback.c` counted from the moment the
network went, so it published the will up to a keepalive later than a real broker, and its
tests held that. It now counts as a broker does: one and a half keepalives after the last
packet it heard from the client -- its CONNECT, a SUBSCRIBE, a message the broker took (one
waiting in a stall is not heard) or a ping, which the client sends whenever a keepalive passes
without another packet (`loopback_last_heard_ms()`, the pings worked out rather than stored).
The tests follow: `test_telemetry_loopback` checks the will after the CONNECT, a message, a
message held by a stall, the pings, and a drop just before and at a ping -- half a keepalive to
one and a half after the drop; `test_reporter`'s outage expects the will 90 s after the last
state message, 30 to 90 s after the drop; `tests/cli/simulator_telemetry.cmake` finds the last
message before its outage (585 s) and expects the will 90 s later (675 s, where the model had
690 s). Each of the three was seen failing against the old model, and the `port_telemetry`
contract, which waits one and a half keepalives after its cut, passes either way. Nothing the
reporter decides depended on the difference.

**Still to see**: the history graphs and the long-term statistics (Developer tools,
Statistics, after the first full hour); a broker with a password, `--mqtt-user`
and `HAC_MQTT_PASSWORD` (seen only in `tests/cli/simulator_broker.py`); and Home
Assistant on its own machine with the Mosquitto add-on, the way the board will meet it.
