# 32. Wi-Fi on the board

Date: 2026-10-21

## Status

Accepted. Phase 4e, branch `phase-4e-wifi`. Verified on the host: the
station's decisions (`platform/esp32_t5s3/adp_net_pure.c`) by unit tests --
the backoff to its ceiling, the timeouts, a lost link sought at once, refused
credentials never given up, every event outside its attempt ignored -- under
ASan and UBSan too. The firmware, its BSEC variant and the test app in both
variants build from fresh sdkconfigs without credentials; the firmware also
with a dummy overlay in a scratch build directory, which the image then
carried, and against which the build refused a stale sdkconfig and warned
about one that named a network no overlay gave.

**On the board, 2026-10-21** (ELF `4734936f1...`, the BSEC image, two boots,
24 minutes up): both boots ran through with no brownout and no interrupt
watchdog; the radio started 40-60 ms after the first frame and the link was up
6.7-6.9 s after `up()`, at the first attempt; EPDiy's draw times with the radio
on are the old ones to the millisecond, the partial redraw that ran during the
association and DHCP included; GPIO14's ADC2 read 32 of 32 beside an idle link.
Internal RAM with the link up: 56.7 KB free, 42.7 KB at the least -- Wi-Fi's
run-time share 68 KB, above the estimate below. **Changed the same day** at
the owner's word, before MQTT adds its own share: the Wi-Fi driver's IRAM
optimisations off (17,564 bytes of static internal RAM back, below), and a
restart no longer logged as a lost link ("A restart", below). The rest --
many boots, recovery, a day's BSEC and heating -- is
`docs/hardware/board_notes.md`'s open item "Wi-Fi on the board".

Implements ADR 0030's "a Wi-Fi adapter behind `port_net`, waking the loop on
every change". The MQTT adapter, and with it the reporter on the board, are
the next sub-phase's.

## Context

ADR 0030 gave the network a port, `port_net`, and the simulator an
implementation; ADR 0031 put the link on the screen. The board composes no
reporter yet (`.reporter = NULL`), because it has neither a station nor an
MQTT client. This sub-phase is the station alone: a link, kept, and seen.

What shapes it:

- **The board browns out when its current jumps.** Every boot reset of phases
  3a to 3f came at the moment `PWR_EN` raises the switched `3V3` -- the panel's
  boost and the sensors switching on at once -- and phase 3f's interrupt
  watchdog resets came from the octal PSRAM, a 3 V part on `VDD3V3`, not
  riding out the dip (`board_notes.md`, open item "A reboot loop when the
  switched 3V3 comes up"). The radio is the largest load the board has yet
  had: its RF calibration at start, and every transmit burst after, draw
  hundreds of milliamps (the ESP32-S3 datasheet's current tables; nothing
  measured here).
- **Requirements 15.1**: a Wi-Fi failure must not block the measurements.
  **11.2**: on USB power Wi-Fi is always, or mostly, on.
- **The panel's row output is timed by software between hardware pulses**
  (EPDiy's row-by-row i80 method, ADR 0007), so whatever delays it stretches a
  frame.
- **The repository is public**, and an ESP-IDF image carries its Kconfig
  strings in plain text.

## Decision

### The station behind `port_net`

`platform/esp32_t5s3/adp_net.c` is `port_net` over ESP-IDF's Wi-Fi station.
Every decision it makes -- when to try again, what gives an attempt up, which
of the driver's events belong to the attempt under way -- is
`adp_net_pure.c`, free of ESP-IDF headers and tested on the host, as
`adp_i2c_pure.c` is (ADR 0011). Its numbers are in one header,
`adp_net_limits.h`, each marked CHOICE or ESTIMATE.

The machine has six phases, each one of the port's states:

| Phase | | `net_state_t` |
|---|---|---|
| `IDLE` | not asked for | `NET_DOWN` |
| `STARTING` | the radio starting | `NET_CONNECTING` |
| `JOINING` | an attempt: scan, association, handshake, DHCP | `NET_CONNECTING` |
| `UP` | an address | `NET_UP` |
| `WAITING` | the backoff between attempts | `NET_CONNECTING` |
| `FAILED` | the radio would not start: given up until `up()` | `NET_FAILED` |

**A task of its own owns the driver.** `up()` and `down()` record what is
asked and wake it; they never call the driver, whose start alone calibrates
the radio. ESP-IDF's event task posts the driver's events into the task's
queue. The task feeds the machine, does what it answers (`esp_wifi_start()`,
`_connect()`, `_disconnect()`, `_stop()`), reads the signal strength every 10 s
while up, and publishes what the port shows under a spinlock held for a few
bytes' copy: `state()`, `rssi_dbm()` and `info()` never wait. Until the task
has acted on a request the port shows the request -- `NET_CONNECTING` after
`up()`, `NET_DOWN` with no address after `down()` -- so no state from before
a request contradicts it. The task wakes the event loop
(`event_loop_post_network()`) whenever the state shown changes.

**Stale events are ignored, not filtered.** The driver posts its events in
order, through one event task, and an attempt begins only at the radio's own
`STA_START`: a disconnection that `esp_wifi_stop()` posted, or an address
from before a `down()`, arrives while the machine is starting or idle, where
it means nothing.

### Reconnecting

- **A link that was up and is lost is sought again at once**: the access
  point was there a moment ago.
- **A failed attempt waits the backoff**, 1 s doubling to 30 s, which starts
  over once the link is up (`ADP_NET_BACKOFF_*`, CHOICES). An attempt is
  itself a scan of every channel, about 2.5 s of receiving: at the ceiling an
  access point gone for hours costs a scan or two a minute, and the link is
  back within 30 s of the access point.
- **An attempt with no address within 30 s is given up** and begun again
  after the backoff (`ADP_NET_ATTEMPT_TIMEOUT_MS`, an ESTIMATE). What it
  catches is DHCP that never answers, which lwIP would otherwise retry for
  ever with the link associated.
- **Refused credentials are never given up.** `port_net.h` names "credentials
  refused" among the things `NET_FAILED` is for; this adapter does not use it
  for them. After a power cut the access point and the device start together,
  and an access point still starting can refuse what it accepts a minute
  later; and a WPA2 network refuses a wrong password with a 4-way handshake
  that times out, which a weak signal produces too. A device that gave up on
  either would stay offline until someone restarted it -- in phase 4 nothing
  calls `up()` twice. The refusals are counted: the log and the console's
  `wifi` say "is the password right?" from the second in a row. `NET_FAILED`
  is left for what no retry mends: the radio that will not start.

ESP-IDF's driver does not reconnect by itself (`failure_retry_cnt` 0), so
there is one policy, this one.

The station's own settings, CHOICES: every channel scanned and the strongest
access point of the name taken (`WIFI_ALL_CHANNEL_SCAN`,
`WIFI_CONNECT_AP_BY_SIGNAL`), which a repeater or a mesh needs; nothing weaker
than WPA2 unless the network has no password; WPA3's SAE in both derivations;
protected management frames when offered. Power save stays at ESP-IDF's
station default, minimum modem sleep. DHCP is told the device's name, MQTT's
device id from the station MAC, `hac-` and twelve hex digits
(`mqtt_device_id_from_mac()`, ADR 0030's amendment): the router's client list
shows the name Home Assistant will.

### Credentials

`CONFIG_HAC_WIFI_SSID` and `CONFIG_HAC_WIFI_PASSWORD`, in
`components/hac_platform/Kconfig` so that the firmware and the test app see
the same symbols, as `HAC_BSEC` is in `hac_drivers`'s. Empty, their default,
means no network: the firmware never sets the station up and runs as it did,
and the linker drops the whole Wi-Fi stack -- `libnet80211` and `liblwip` are
absent from such an image's `size-components`.

They are set in **`apps/firmware_esp32/sdkconfig.credentials`**, gitignored,
copied from the committed `sdkconfig.credentials.example`.
`cmake/credentials_idf.cmake` layers it last over the defaults of every build
of the firmware -- the BSEC and calibration variants' `-D
SDKCONFIG_DEFAULTS=...` too -- and of the test app, so no command line
changes, and a deleted sdkconfig gets the credentials back. Like every overlay
it reaches only an sdkconfig that does not hold the symbols yet (CLAUDE.md);
an sdkconfig made before the overlay was written or changed would build an
image for the old network, or none, without a word. So, after `project()`,
the same file compares the two and **refuses to configure while they
disagree**, naming the symbol and never its value; and it warns when an
sdkconfig names a network that no overlay gives (menuconfig), since that
image carries it. A change to the overlay re-runs the configuration, where the
check sees it.

**The driver keeps no copy.** Its init configuration has `nvs_enable = 0` and
its storage is `WIFI_STORAGE_RAM`, whatever an sdkconfig says, so an image
built without credentials, flashed after one with them, finds none left in
the NVS partition. The flash's NVS partition is opened (`nvs_flash_init()`),
never erased, for one thing: ESP-IDF's PHY keeps the radio's calibration data
there, so later starts calibrate in part. Where it cannot be opened, every
start calibrates in full and the log says so; erasing a partition a later
phase will keep settings in is not a network adapter's call.

The password is never logged; the boot line gives its length. An image built
with credentials holds both in plain text: never attach one, an sdkconfig or
the overlay to an issue or a pull request. Changing them without a rebuild --
NVS, BLE or SoftAP provisioning -- is phase 6's.

### When the radio starts

`adp_net_init()` runs in `app_main`, after the switched rail has risen and
settled, after the log's hook is released (`log_tee_hold(false)`), after the
card and touch have started: ESP-IDF's netif, event loop and driver -- their
memory taken, their tasks running, the radio off. The NVS read it does is in
`app_main` on purpose, before the first frame: a flash access disables the
cache, and an IRAM-less interrupt EPDiy's row loop waits on (the RMT's) would
wait with it. It costs the first frame the init's time, logged as `adp_net:
station ready took_ms=`.

**The radio starts once the first frame is over.** The event loop calls the
composition back once its first redraw -- the boot's full refresh, about
3.3 s -- has returned, flushed or not (`after_first_frame`, `event_loop.h`),
and the composition calls `up()`. By then the rail rose seconds ago, and the
next panel drive is a sensor cycle away at least (`min_redraw_interval_ms`,
5 s) unless someone taps. The RF calibration's current, and the flash write
that stores its data at a first start, come in that gap: neither in the rail's
inrush nor during a drive. What this cannot keep apart is everything after: a
transmit burst can come during any refresh or any sensor's measurement. That
is what the board's checks watch. Phase 5's raising of `PWR_EN` after a deep
sleep inherits the same order: the rail, its settling, the first frame, then
the radio.

### Its buffers in internal RAM

`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` stays off, ESP-IDF's default, written
out in `sdkconfig.defaults`. The Wi-Fi driver touches its buffers exactly when
the radio draws most -- a frame is copied out as it is transmitted -- and the
octal PSRAM is the part that did not ride out a dip on `VDD3V3`. Internal
buffers keep the driver's own accesses off PSRAM at those moments. They do not
make PSRAM safe: EPDiy's buffers, the log's ring and the recorder are in it
and are touched whenever their tasks run, during a burst or not. If the
radio's current dips `VDD3V3` far enough, it will show as interrupt-watchdog
resets with Wi-Fi up whatever this setting, and the levers then are the
radio's transmit power (`esp_wifi_set_max_tx_power()`) and the supply, not
where the buffers are.

What it costs, from `idf.py size` with a dummy network against the same tree
without one (2026-10-21):

| | without a network | with one | |
|---|---|---|---|
| Image | 713,913 B | 1,215,097 B | +501,184 |
| Internal RAM, static (DIRAM) | 97,803 B | 148,943 B | +51,140 |
| ... of it code (IRAM) | 71,179 B | 101,363 B | +30,184 |
| ... `.bss` | 13,008 B | 26,144 B | +13,136 |
| ... `.data` | 13,616 B | 21,436 B | +7,820 |

The libraries: `libnet80211` 123.5 KB, `liblwip` 85.0, `libpp` 63.8,
`libmbedcrypto` 63.3, `libwpa_supplicant` 46.8, `libphy` 35.5, `libnvs_flash`
15.7, `libesp_netif` 9.0, `adp_net` 7.3, the rest a few KB each. Of the
static internal RAM, `libpp` takes 18.2 KB, `libnet80211` 12.3, `libphy` 8.4.
The 30.2 KB of code is ESP-IDF's `CONFIG_ESP_WIFI_IRAM_OPT` and
`_RX_IRAM_OPT`, its defaults. At run time the driver adds its static receive
buffers (10, about 16 KB), its dynamic ones as traffic needs them, and the
stacks of its task, lwIP's, the default event loop's and this adapter's
(6 KB): an ESTIMATE of 40-60 KB more, to be measured against phase 3g's
176,143 bytes free after the card's mount (`adp_net: memory with the link up
free_internal=`). **Measured 2026-10-21: 68 KB** -- 56,703 bytes free with the
link up, 42,691 at the least, about 54 KB of it taken before the radio
starts. If what is left is too little, the first lever is the IRAM
optimisation (about 30 KB back, a slower link a message a minute does not
notice), then fewer static receive buffers, and only then PSRAM, with this
section argued again.

**The first lever is pulled** (2026-10-21, after the measurement):
`CONFIG_ESP_WIFI_IRAM_OPT` and `_RX_IRAM_OPT` off in `sdkconfig.defaults`, and
`adp_net.c` refuses to build with an sdkconfig that has either on, since a
default does not reach an existing one. `idf.py size`, with a network: static
internal RAM 148,943 -> 131,379 bytes, 17,564 back -- not the 27 KB and more
ESP-IDF's help texts suggest, since what the driver must run from IRAM stays
there. Wi-Fi's static share is now 33,576 bytes. The cost is the link's
throughput and latency under load, which a message a minute does not reach;
the code runs from flash through the same cache EPDiy's and BSEC's do.
On the board (ELF `8baab9290...`, three boots): 74.2 KB free with the link
up, 60.2 KB at the least, and the link as fast as before, 6.8-6.9 s.

The image, 1.2 MB, is 29 % of the 4 MB app partition.

### The cores, and the panel's row output

EPDiy renders on two tasks at the highest priority there is (24):
`epd_prep` 1, on core 1, outputs the rows; 0, on core 0, prepares them. The
row loop spins on two flags set by interrupts -- the LCD_CAM transfer's end,
the RMT's CKV pulse's end -- which EPDiy's init allocated from `app_main`, so
on core 0. ESP-IDF's Wi-Fi task runs on core 0 at 23 and takes its interrupt
there; lwIP's task is pinned to core 0 too (`sdkconfig.defaults`, 18), as is
this adapter's (4). So nothing of Wi-Fi preempts either EPDiy task, and
nothing of it runs on core 1 at all; what it can do is hold core 0 in an
interrupt for microseconds while one of the two completion interrupts is
pending. The CKV pulse is the RMT's, timed in hardware: a late flag stretches
the gap between two rows, not the pulse that drives one. Whether the frame's
longer total shows on the glass -- the panel's pixels hold their charge
between rows, so a frame's length is part of the waveform -- is the board's
to show: EPDiy's per-update timing lines against ADR 0008's table, and the
greys by eye.

### What the screen shows

Nothing new. The status bar's word and the diagnostics screen's network
lines come from the reporter (ADR 0031), which the board does not compose
yet: the bar stays empty, and the diagnostics screen still says `Wi-Fi off`
and `MQTT off` while the station is up. Showing the station without the
reporter would mean changing the diagnostics view for one sub-phase; the
console's `wifi` is this sub-phase's view instead, and both follow the link
once the MQTT adapter composes the reporter.

### A restart

`esp_restart()` -- the console's `restart`, later an OTA's -- calls the
shutdown handlers, and the one `esp_netif_create_default_wifi_sta()`
registers stops the driver, whose disconnection the adapter took for a lost
link: it tried to connect again and logged both. The adapter now registers a
shutdown handler of its own, after that one, so that it runs first
(`esp_restart()` calls them last registered first): it sets a flag, and from
then on the driver's events are dropped and the task does nothing. A panic
resets without the handlers, and its log ends where it ends.

### The console

`wifi`: the state, the network's name, the signal, the address and the
gateway, the channel and the access point; how long up, or how long sought,
the attempts failed in a row and when the next comes; connections, losses,
attempts, failures and timeouts since boot and the last reason, by name and
code; the station's MAC, the device id, the task's stack low-water mark and
the events the queue dropped. Never the password.

### The contract on the board

`tests/contract/esp32/test_port_net_contract.c`: the shared body against the
station, joining the network the overlay names, with `cut` and `restore` NULL
-- the test app cannot take the access point away, so those checks say "not
exercised". About two minutes of real time. An image without credentials
reports the case IGNORED and says why. It runs the contract at the event
loop's priority, above the station's task, as the firmware's caller is: at
the test task's own, below it on the same core, the first `up()` was
preempted by the radio's start it had just asked for, and its timing check
failed on time `up()` never spent waiting (the board, 2026-10-21).

## Consequences

- **A build with a network is a build with credentials.** The overlay is the
  only place they live; an image without them is the old firmware and links
  no Wi-Fi at all. CI has no overlay and no firmware job: the Wi-Fi stack is
  compiled there never and linked only in a build with an overlay -- this
  branch linked it once, with a dummy network, and a missing component would
  show only there.
- **Existing sdkconfigs needed regenerating** for the new defaults (lwIP's
  core, no SoftAP) and the new symbols; this branch's firmware, BSEC and test
  app directories were (the old files kept as `sdkconfig.old`). Any other --
  the calibration variants', the slideshow's and the photo frame's -- needs
  the same before its next build.
- **A wrong password never stops the trying**: at the backoff's ceiling, a
  scan and a refused handshake every 30 s, for as long as it is wrong.
- **The first frame waits for the station's set-up** in `app_main`, logged.
- **The board owes**: many boots with the radio, no brownout and no interrupt
  watchdog; recovery from an access point rebooted and gone; internal RAM and
  `size-components`; EPDiy's timings and the glass with Wi-Fi up; BSEC's late
  calls per day; the BME68x's heating against the SCD41 with Wi-Fi and
  without -- requirements 12's warning, and `HAC_BSEC_DEVICE_HEAT_C`'s first
  measured value; and, closing another open item, GPIO14's ADC2 read with
  Wi-Fi on (`board_notes.md`, open item "Wi-Fi on the board").
