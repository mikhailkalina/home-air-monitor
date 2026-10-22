# 33. MQTT on the board

Date: 2026-10-22

## Status

Accepted. Phase 4g, branch `phase-4g-board-mqtt`. Verified on the host: the
adapter's decisions (`platform/esp32_t5s3/adp_mqtt_pure.c`) by unit tests --
the broker's URI and its refusals, the queues' indices, a received message put
together from pieces and cut at the port's limit, which session an esp-mqtt
event belongs to, what `is_connected()` says when the network goes, what the
adapter's task does next -- under ASan and UBSan too. The firmware, its BSEC
variant and the test app in both variants build from regenerated sdkconfigs
with the owner's network and no broker; the firmware and the test app also
with a dummy broker in a scratch build directory, which linked esp-mqtt and
carried the broker's address.

**On the board, 2026-10-22** (ELF `6211314dd...`, the BSEC image, a mosquitto
2.0.21 on a Raspberry Pi Zero W with users and an ACL, 12 minutes): the first
session at the first attempt, `online` and nine configs; about 10 KB of
internal RAM with a session (51,799 bytes free against 61,823 before it),
under the estimate below; the event loop's longest call into the port 138-204
us; esp-mqtt's outbox full at each session's start, nothing refused. A broker
restart recovered in 15 s, 10 of them the reporter's connect timeout on an
attempt esp-mqtt knew had failed within half a second -- the port change
weighed below, its first board figure. A `restart` left the will. Still owed:
Home Assistant on that broker, its restart, Wi-Fi gone for an hour, the day
unattended, the test app (`docs/hardware/board_notes.md`, open item "MQTT on
the board (phase 4g)").

Implements ADR 0030's "What the board's adapters inherit" for MQTT, the
board's half of what ADR 0030's amendment gave the simulator. With it the
board composes the reporter, and ADR 0031's status bar and diagnostics
screen show the board's own link.

## Context

ADR 0030 gave telemetry its ports and the reporter; its amendment put the
simulator in front of a real broker and a Home Assistant; ADR 0032 gave the
board Wi-Fi and ADR 0023's amendment SNTP. The board still composes no
reporter (`.reporter = NULL`): it has no MQTT client. Requirements 16,
"Phase 3", asks that the device appear in Home Assistant by itself, with
retained state, an availability topic and long-term statistics.

What shapes the client:

- **The event loop never waits.** It runs the sensors, the redraws and the
  card's posts on one task (ADR 0024: "the event loop only posts what
  changed"); a wait for the network there is a late BSEC call, a late redraw,
  a tap that takes seconds.
- **esp-mqtt**, ESP-IDF 5.5's MQTT client (`components/mqtt/esp-mqtt`,
  Apache-2.0), runs a task of its own and reconnects and resends by default.
  `port_telemetry` promises neither (ADR 0030): the reporter decides when to
  connect, and every session starts afresh.
- **Internal RAM.** 72.5 KB free with the link up and SNTP (phase 4f, the
  board), 60 KB at the least; the Wi-Fi driver's buffers stay internal and the
  octal PSRAM is kept out of the radio's way (ADR 0032).
- **The repository is public**, and an image carries its Kconfig strings in
  plain text.

## Decision

### `platform/esp32_t5s3/adp_mqtt.c`, `port_telemetry` over esp-mqtt

Its decisions are `adp_mqtt_pure.c`, free of ESP-IDF headers and tested on the
host, as `adp_net_pure.c` is; its numbers are one header,
`adp_mqtt_limits.h`, each marked CHOICE or ESTIMATE. Plain MQTT 3.1.1 over
TCP, QoS and retain as the reporter asks (ADR 0029).

Named `adp_mqtt`, not `adp_telemetry_mqtt` as `docs/architecture.md` first
listed it: the simulator's libmosquitto adapter has that name -- its header,
`platform/host/adp_telemetry_mqtt.h`, and its functions,
`adp_telemetry_mqtt_*` -- and the board's pure half is compiled on the host,
where a test that included both directories, or linked both, would meet two
of each.

### A task of its own, the only caller of esp-mqtt

ADR 0030 read esp-mqtt's header and settled on `esp_mqtt_client_enqueue()`,
"a publish that only enqueues", since `esp_mqtt_client_publish()` writes to
the socket from its caller and "might block for several seconds". **Its source
says more** (`mqtt_client.c` in ESP-IDF 5.5, read 2026-10-22): every public
call takes the client's API lock (`MQTT_API_LOCK`, `portMAX_DELAY`),
`esp_mqtt_client_enqueue()` included, and esp-mqtt's own task holds that lock
through each step of its loop -- the name lookup, the TCP handshake and the
wait for the CONNACK while connecting, each up to the network timeout; a
write, a resend and a keepalive ping while connected, each up to the same
timeout into a link that has gone. So `enqueue()` called from the event loop
waits whenever esp-mqtt waits for the network: seconds at a time while a
broker is unreachable or Wi-Fi has just gone. Subscribing writes from the
caller as well. And stopping a client waits for esp-mqtt's task to end.

So the event loop calls none of it. **The adapter has a task of its own**
(`hac_mqtt`, 6 KB, priority 4, core 0), and it alone calls esp-mqtt:

- `publish()` and `subscribe()` copy the message -- topic, payload, QoS,
  retain, the session it belongs to -- into a ring of slots in PSRAM and wake
  the task. The task hands each to esp-mqtt with `esp_mqtt_client_enqueue()`,
  `store` set, never `esp_mqtt_client_publish()`; a subscription with
  `esp_mqtt_client_subscribe_single()`. In order, one ring for both.
- `connect()` and `disconnect()` record what is asked and wake the task, which
  makes and starts a client, or stops and destroys one.
- The ring's indices move under the adapter's spinlock, a few words at a
  time; a slot is filled and emptied outside it. No copy is made with
  interrupts off, on either core.

What the network makes esp-mqtt wait, the adapter's task waits; the event
loop's calls cost a copy. The console's `mqtt` gives the longest of them, as
`rec` gives the card's longest post.

esp-mqtt itself sends what is queued one message per turn of its loop, and
each turn ends waiting up to a second for something to arrive
(`MQTT_POLL_READ_TIMEOUT_MS`): a message queued while it waits goes within
that second, and the next as the previous one's PUBACK wakes it. A session's
start of twelve QoS 1 messages takes a second and twelve round trips on the
LAN; a state message a minute, a second at most.

### One client per session

The task makes an esp-mqtt client for each session and destroys it when the
session ends, as the simulator's libmosquitto adapter makes one per session:

- **its reconnection off** (`network.disable_auto_reconnect`): the reporter
  decides;
- **nothing of one session in the next**: the outbox dies with its client,
  so nothing is resent into a later session, which starts with "online", the
  discovery configs and the newest state anyway;
- **a client ID, a keepalive and a will per session**, from `connect()`'s
  arguments, set in a fresh configuration rather than changed in place.

Clients are numbered by session. esp-mqtt's events carry the number of the
client that posted them, and one from a client the caller has let go of
changes nothing; a client is destroyed before the next is made, so a session
that `disconnect()` ended says goodbye before the next says hello.

The cost is a task created and freed per session -- esp-mqtt's 6 KB stack and
its buffers, internal -- a handful of times a day.

### Bounds

- **The ring**: 16 slots (`ADP_MQTT_OUT_SLOTS`, CHOICE), each a topic and a
  payload at their longest, 27 KB of PSRAM with the rest of the adapter's
  buffers. A session's start is twelve: the subscription, "online", nine
  discovery configs and the state, accepted in one reporter step. Past 16,
  `publish()` says `HAL_ERR_NO_MEM`, and the reporter tries again a second
  later (`REPORT_RETRY_MS`).
- **esp-mqtt's outbox**: 4 KB (`outbox.limit`, `ADP_MQTT_OUTBOX_LIMIT_BYTES`,
  CHOICE), in internal RAM, where QoS 1 messages wait for their PUBACK. When it
  is full the task leaves the rest in the ring and goes on at the next
  acknowledgement, or 200 ms later: on a LAN, milliseconds.
- **The inbox**: 8 received messages (`ADP_MQTT_INBOX_SLOTS`), the port's
  minimum; one longer than esp-mqtt's 512-byte input buffer arrives in pieces
  and is put together, and kept to the port's 256 bytes, marked truncated.
- **esp-mqtt's buffers**: 1,280 bytes out -- the longest message whole, so
  nothing goes in fragments -- and 512 in (CHOICES). Its task: ESP-IDF's
  defaults, 6 KB and priority 5, pinned to core 0 (`sdkconfig.defaults`), an
  ESTIMATE until its stack's low-water mark, which the adapter takes from
  inside esp-mqtt's event handler.
- **esp-mqtt's network timeout**: 5 s (CHOICE), half the reporter's 10 s for
  an attempt, so that an attempt esp-mqtt gives up is over before the
  reporter gives up on it.

### Events

esp-mqtt's handler runs on esp-mqtt's task. `CONNECTED` makes the session
accepted; `DISCONNECTED` ends it -- lost if it had been accepted, refused if
an `ERROR` event carried a CONNACK refusal first, else no connection -- and
the transport's errno and the CONNACK's code are kept for the log and the
console. `DATA` goes into the inbox while the caller holds that session.
`PUBLISHED` counts an acknowledgement and wakes the task, for what waits. A
session that began or ended, and a message that arrived, wake the event loop
through the composition's waker, `event_loop_post_network()`. The handler
does nothing else: it logs nothing; the task logs why a session ended, each
reason once until a session works again, as the host adapter does.

### The network

The adapter is given `port_net`. `connect()` needs the link up
(`HAL_ERR_NOT_READY` otherwise), and a session whose network has gone is over
at the next call into the port -- the port's "a drop the adapter has
noticed" -- without waiting for esp-mqtt's keepalive to find out, which takes
up to a keepalive and a half. Its client is then stopped like any other.

### How a session ends

- **`disconnect()`**: esp-mqtt sends a DISCONNECT when it still thinks the
  session up, and closes the socket: the broker drops the will.
- **Wi-Fi gone**: the reporter ends the session, and the client is stopped
  the same way -- its DISCONNECT goes into a dead link. The broker publishes
  the will once it has heard nothing for one and a half keepalives, 90 s;
  Home Assistant shows the device unavailable. If the link is back before
  that, and before lwIP's IP-lost timer (120 s) has dropped the old socket,
  lwIP's retransmission may deliver the DISCONNECT late, and the broker drops
  the will for a device that is back within seconds: Home Assistant never
  shows the gap. Stopping without a DISCONNECT would take esp-mqtt's internals
  -- its public stop sends one whenever it thinks itself connected -- for a
  difference no one sees.
- **The broker gone or refusing**: esp-mqtt's `DISCONNECTED`; the log says
  why.
- **A reset or a power cut**: no DISCONNECT; the will, 90 s later at most.

### Configuration

Four symbols in `components/hac_platform/Kconfig`, so that the test app sees
them too, set in the gitignored overlay `sdkconfig.credentials` beside the
Wi-Fi network (ADR 0032), never in anything committed:

- `CONFIG_HAC_MQTT_BROKER_URI`: `mqtt://<host>[:<port>]`. Empty, the
  default, means no MQTT: the firmware runs as it did, Wi-Fi and SNTP
  included, and the linker drops esp-mqtt. `mqtts://`, a user or a password
  in the URI, a path: refused at boot, with the reason in the log. The host
  is an address or a name the router's DNS resolves; lwIP does no mDNS, so
  not `.local`.
- `CONFIG_HAC_MQTT_USER`, `CONFIG_HAC_MQTT_PASSWORD`: empty for an anonymous
  broker. The password goes only with a user name, and is never logged; the
  boot line gives its length.
- `CONFIG_HAC_MQTT_TOPIC_PREFIX`: `homeair` by default, requirements 10.1's
  example and the simulator's; a household with its own sets it in the
  overlay.

`cmake/credentials_idf.cmake` checks all four as it checks the network's: a
build refuses to configure while the overlay and the sdkconfig disagree, and
warns about an sdkconfig that holds a credential no overlay gives.

The device id is the station MAC's, `hac-` and twelve hex digits
(`mqtt_device_id_from_mac()`, ADR 0030's amendment), as DHCP already tells the
router. The discovery's device: "Home Air Monitor", from "home-air-monitor",
model "LILYGO T5 4.7" e-Paper S3", hardware version the silkscreen's
(`BOARD_SILKSCREEN_REVISION`), software version the image's app description
-- `git describe` at build time -- and the same in the origin block. Every
config fits: 851 bytes at the longest with these strings, 892 with the
longest prefix the Kconfig allows, against 1,024 (measured on the host).

### Plain MQTT on the LAN: no TLS

The URI refuses `mqtts://`, and esp-mqtt's TLS and websocket transports are
not linked (`CONFIG_MQTT_TRANSPORT_SSL`, `_WEBSOCKET` off in
`sdkconfig.defaults`; `adp_mqtt.c` refuses an sdkconfig with either on).

**What TLS would cost.** An Mbed TLS session in internal RAM: ESP-IDF's
defaults keep a 16 KB input record buffer and a 4 KB output buffer for it,
and its handshake works through tens of kilobytes more at its peak -- against
the 72.5 KB free with the link up, before MQTT's own share (below). Mbed TLS
can allocate from PSRAM instead (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`), which
puts the network's buffers back where ADR 0032 took them out of. And a broker
on the LAN has a self-signed certificate, whose CA the image would carry and a
renewal would have to reach. The code itself is already in the image (below).

**What the owner accepts without it**: the MQTT user's password crosses the
LAN in clear text at every connection, and so do the readings; anyone on the
same network can read them, and, with the password, publish as the device --
its state, its availability -- unless the broker's ACLs keep that user to the
device's own topics. A password used nowhere else, and such an ACL, are the
mitigation, not a cipher. TLS is a later phase's, with this section argued
again.

### The composition

`main.c`'s `start_telemetry()`, for an image with a network and a broker,
after the sensors -- whose capabilities say which entities discovery announces
-- and before the event loop: the topics, the client and its task, the
reporter, given to the event loop (`.reporter`). The reporter connects once
the station's link is up, which is after the first frame (ADR 0032), so the
first session comes a few seconds after it. From then on the status bar says
`online`, `no broker` or `no Wi-Fi` once the link has held it a minute, and
the diagnostics screen shows both network lines (ADR 0031).

The event loop logs each session's announcements at INFO and each state
message at DEBUG, as before (`log_report()`). The card's log keeps INFO, so a
day's publishing is read from **an hourly line**, `event_loop: published in
the hour state= heartbeats= discovery= sessions= lost= refused=`
(`EVENT_LOOP_REPORT_LOG_INTERVAL_MS`, CHOICE): 24 lines a day.

### The console

`mqtt`: the session -- connected, connecting, over or idle, its number --
the broker and the user, never the password; why the last session ended, with
the CONNACK's reason or the socket's errno; published, subscribed, refused
(the ring full), forwarded to esp-mqtt, acknowledged, discarded with a
session, received and dropped; what waits -- messages and bytes in the ring,
bytes in esp-mqtt's outbox, received messages not yet polled; attempts,
sessions and how each failure ended since boot; the event loop's longest call
into the port and which call it was; the two tasks' stacks.

### The contract on the board

`tests/contract/esp32/test_port_telemetry_contract.c`: the shared body against
`adp_mqtt` and the owner's broker, over the Wi-Fi station the `port_net` case
shares (`test_station.c`: ESP-IDF's driver is one, and `adp_net_init()` takes
it once). Its client id is `contract-<MAC>` and its topics are under
`hac-contract/board-<MAC>`, cleared before it ends -- never the device's own,
so the board's place in Home Assistant is not touched. The network can be
taken away under a session -- the station's `down()`, then `up()` -- so the
drop and its will are exercised; the broker cannot be made to stop taking
messages, so `stall` is NULL and the full queue says "not exercised". The
contract runs on a task of the event loop's priority on core 1, where the
firmware's caller runs, away from the station's, the adapter's and esp-mqtt's
tasks on core 0: its timing checks time the calls, not a preemption (ADR
0032's lesson from the `port_net` case). An image without a broker reports
the case IGNORED and says why. The first run on the board failed once, on a race
between cases: the `port_net` case ended with a `down()` the station's task had
not acted on, so `up()` was answered at once with a link about to drop and
esp-mqtt's single attempt met a stopping radio. The shared station now waits for
the task's phase to be idle before it asks for the link; the adapter was not at
fault, and the firmware calls `up()` once. The second run passed in full.

### A port change weighed, and not made

ADR 0030's amendment found the reporter waiting out its 10 s connect timeout
on an attempt the adapter knew had failed, and left "a way for
`port_telemetry` to say that an attempt is over" to be weighed with this
adapter. It knows as quickly: `DISCONNECTED` for an attempt. The cost of not
telling is 10 s per refused attempt, only while a broker refuses or is
absent, inside a backoff that reaches two minutes anyway; telling would change
the port, its contract, the reporter and three adapters. Not made: the
board's days show whether it matters.

### What it costs

From `idf.py size`, the firmware with the owner's network and a dummy broker
against the same tree with the network and no broker (2026-10-22):

| | network, no broker | network and broker | |
|---|---|---|---|
| Image | 1,239,197 B | 1,363,697 B | +124,500 |
| Internal RAM, static (DIRAM) | 133,371 B | 137,275 B | +3,904 |
| ... `.bss` | 28,056 B | 31,616 B | +3,560 |

The static RAM is the reporter (2,376 bytes), the topics and the adapter's
state; its buffers are in PSRAM (about 27 KB). The image: `libmbedtls` 29.6 KB,
`libmbedcrypto` 16.2 KB more, `libmbedx509` 8.6 KB -- Mbed TLS's TLS layer,
linked by `esp-tls` (6.4 KB), through which ESP-IDF's plain TCP transport
connects and closes, and never run -- `libmqtt` 14.8 KB, lwIP's TCP client
11.6 KB, this adapter 7.3 KB, `tcp_transport` 2.8 KB, `http_parser` 2.1 KB
(esp-mqtt's URI parser), and about 15 KB of read-only data the size tool
attributes to `esp_app_format`. 1.36 MB is 33 % of the 4 MB app partition.

At run time, with a session: esp-mqtt's task (6 KB) and this adapter's (6 KB),
esp-mqtt's client, configuration, buffers and event loop (about 3 KB), the
TCP socket, and up to 4 KB of outbox -- an ESTIMATE of 20-25 KB of internal
RAM, to be measured against the 72,503 bytes free with the link up of phase
4f (`adp_mqtt: memory with a session free_internal=`).

## Consequences

- **A build with MQTT is a build with a broker in the overlay**; without one
  the image is phase 4f's, and links no MQTT. CI has no overlay: esp-mqtt is
  compiled there never, and linked only in a build with a broker -- this
  branch linked it with a dummy one.
- **Existing sdkconfigs needed regenerating** for the new defaults (esp-mqtt's
  transports, its task's core) and the new symbols; this branch's firmware,
  BSEC and test app directories were, the old files kept as `sdkconfig.old`.
  Any other that links `hac_platform` -- the calibration variants', the
  slideshow's and the photo frame's -- needs the same before its next build:
  `adp_mqtt.c` refuses a stale one.
- **The event loop's calls into MQTT cost a copy**, measured; whatever the
  network makes esp-mqtt wait, the adapter's task waits.
- **The password crosses the LAN in clear text**, accepted for now.
- **The board owes** (`board_notes.md`, open item "MQTT on the board (phase
  4g)"): the device in Home Assistant beside the simulator, its values those
  of the screen; a broker restart, a Home Assistant restart and Wi-Fi gone
  for an hour, with the measuring, the screen and the card unaffected and the
  longest call in microseconds; esp-mqtt's stack and the internal RAM with a
  session; and a day unattended, read from the card's `log/` -- publishes and
  reconnections, BSEC's late calls and the full refreshes against earlier
  days -- with Home Assistant's long-term statistics for every measurement
  entity, requirements 16's acceptance for "Phase 3".
