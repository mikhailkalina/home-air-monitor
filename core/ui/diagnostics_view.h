// SPDX-License-Identifier: Apache-2.0
//
// The diagnostics screen's view model: what the core and the composition
// already know about the device's health, as text and nothing that still
// needs a decision -- the same split as view_model.h, so "what does this say
// when the BME68x keeps failing" is a unit test over plain data
// (tests/unit/test_diagnostics_view.c).
//
// It shows (docs/adr/0025-touch-input-gt911.md):
//
//   * each sensor source the manager drives: its name, its state --
//     "starting" until its first init() succeeds, "running", "failing" once
//     a read or start failed and none succeeded since, "restarting" while it
//     waits out the backoff before init() again -- its failures and its
//     restarts since boot;
//   * each reading the home screen shows, and the gas resistance under it:
//     its status in the home screen's words, its accuracy where the source
//     gives one, and which source the merge took it from -- a temperature
//     "from scd4x" is the fallback at work;
//   * the network (docs/adr/0031-the-network-on-the-screen.md): the Wi-Fi
//     link's state, the network's name, the signal and the device's address;
//     the MQTT session's state, the state messages sent, the messages the
//     adapter refused, and the local time the last state message went;
//   * whether the wall clock is set, and from where when the composition
//     says;
//   * the recorder's state;
//   * the devices beside the sensors that no sensor manager drives -- the
//     touch controller and the RTC -- as the composition finds them: a state
//     in the sources' words where they fit ("starting", "running") and words
//     of their own where they do not ("asleep", "missing", "no time"), the
//     transfers that failed since boot, and one detail each -- the bus
//     address the touch controller answers at, which moves between its two
//     (board_notes.md, "The touch panel on the board"), and the RTC's drift
//     against the network's time
//     (docs/adr/0041-touch-rtc-and-battery-on-the-diagnostics-screen.md);
//   * the battery: the status bar's percentage and the millivolts behind it.
//
// The screen is a developer's, up for minutes while someone tests the device,
// and it is live: everything it draws redraws as soon as it changes
// (diag_view_equal() compares all of it), with a partial refresh at once --
// not held to a sensor cycle, not counted against the budget -- and the loop
// looks at it every second while it is up
// (docs/adr/0038-the-diagnostics-screen-is-a-developers.md and its
// amendment). Partial refreshes are not saved on it: the full refresh home
// clears what they leave. What moves most often is the signal, which the
// network adapter reads every 10 s, and the battery's millivolts, a median
// that moves at every burst, 12 s apart: a redraw each, every 10 s or so.
//
// What it shows is chosen for what it says, not for what it costs. The slot
// states fold IDLE and MEASURING into "running": they are the phases of every
// measurement, not the source's health, and a forced-mode BME68x passes
// through both every cycle -- the word would flicker, not inform. "joining"
// covers a whole outage, every attempt and the backoff between them, as one
// state. The link the status bar shows (core/app/link_hold.h) is not on
// this screen: its word is the MQTT line's state, said twice, a minute late.
// Neither is it compared: compared and not drawn, a change of it made a
// redraw due whose picture was the one on the glass -- an empty diff, tried
// again every second until the screen timed out
// (docs/adr/0034-a-change-of-screen-is-a-full-refresh.md, amending
// docs/adr/0031-the-network-on-the-screen.md).

#ifndef HAC_CORE_UI_DIAGNOSTICS_VIEW_H
#define HAC_CORE_UI_DIAGNOSTICS_VIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "app/sensor_manager.h"
#include "domain/civil_time.h"
#include "domain/measurement.h"
#include "port_net.h"
#include "recording/recorder.h"
#include "reporting/reporter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DIAG_MAX_SLOTS SENSOR_MANAGER_MAX_SOURCES

// A source's name as the screen keeps it; the renderer shortens it further
// to fit its column.
#define DIAG_NAME_MAX 24

// Counts are shown up to this, then as "999+": the widest text a count can
// be, so the layout holds whatever the device has been through.
#define DIAG_COUNT_SHOWN_MAX 999u
#define DIAG_COUNT_TEXT_MAX 8  // "999+"

#define DIAG_LINE_MAX 40

// The network's name as the screen keeps it: every octet the port gives,
// those outside printable ASCII as '?'; the renderer shortens it to its
// column.
#define DIAG_SSID_MAX (PORT_NET_SSID_MAX + 1u)

// The network section's short texts: "-127 dBm", "255.255.255.255",
// "sent 999k+", "refused 999+", "last 15.10 23:58".
#define DIAG_SHORT_TEXT_MAX 20

// The state messages sent are counted exactly up to this, then in whole
// thousands -- "sent 12k" -- up to DIAG_SENT_THOUSANDS_MAX of them, then
// "sent 999k+": a device sends some 1,500 a day, and the column has room for
// four digits and a suffix.
#define DIAG_SENT_EXACT_MAX 9999u
#define DIAG_SENT_THOUSANDS_MAX 999u
#define DIAG_SENT_PER_THOUSAND 1000u

// The RTC's drift is shown with a tenth of a ppm up to this, in whole ppm up
// to DIAG_DRIFT_WHOLE_MAX_PPM, and at that past it: a crystal is off by tens
// of ppm, and the column has room for "drift -99.9 ppm" and "drift -999 ppm".
#define DIAG_DRIFT_TENTHS_MAX_PPM 99.9f
#define DIAG_DRIFT_WHOLE_MAX_PPM 999.0f

// How a device beside the sensors is, as the composition finds it.
typedef enum {
    DIAG_DEVICE_NONE = 0,  // none in this build: no line
    DIAG_DEVICE_STARTING,  // not looked for yet
    DIAG_DEVICE_RUNNING,   // answering; an RTC holding a time to use
    DIAG_DEVICE_ASLEEP,    // put to sleep on purpose
    DIAG_DEVICE_MISSING,   // not answering where it should
    DIAG_DEVICE_NO_TIME,   // an RTC that answers and holds no time to use
} diag_device_state_t;

typedef struct {
    diag_device_state_t state;
    uint32_t failures;  // reads that failed since boot
    uint8_t address;    // the bus address it answers at; 0 when not known
} diag_touch_t;

typedef struct {
    diag_device_state_t state;
    uint32_t failures;  // writes that failed since boot
    bool drift_known;
    float drift_ppm;  // against the network's time, last measured; positive: it gains
} diag_rtc_t;

typedef enum {
    DIAG_DEVICE_TOUCH = 0,
    DIAG_DEVICE_RTC,
    DIAG_DEVICE_COUNT,
} diag_device_id_t;

typedef enum {
    DIAG_FIELD_CO2 = 0,
    DIAG_FIELD_TEMPERATURE,
    DIAG_FIELD_HUMIDITY,
    DIAG_FIELD_PRESSURE,
    DIAG_FIELD_IAQ,
    DIAG_FIELD_GAS,
    DIAG_FIELD_COUNT,
} diag_field_t;

typedef struct {
    char name[DIAG_NAME_MAX];
    const char *state;  // "starting", "running", "failing", "restarting"
    bool trouble;       // failing or restarting
    char failures[DIAG_COUNT_TEXT_MAX];
    char restarts[DIAG_COUNT_TEXT_MAX];
} diag_slot_view_t;

typedef struct {
    const char *label;           // "CO2", "Temperature", ...
    const char *status;          // "ok", "warm", "stale", "ERR", "n/a" -- the home screen's words
    const char *accuracy;        // "high", "medium", "low", "unreliable", or "-" when not reported
    char source[DIAG_NAME_MAX];  // the name of the slot it came from, "" when none
    bool trouble;                // an error, or a number its source has not calibrated
} diag_field_view_t;

typedef struct {
    const char *name;   // "touch", "rtc"
    const char *state;  // "starting", "running", "asleep", "missing", "no time"
    bool trouble;       // missing, or no time
    char failures[DIAG_COUNT_TEXT_MAX];
    char detail[DIAG_SHORT_TEXT_MAX];  // "at 0x5D", "drift -20.8 ppm", "drift -"; "" for none
} diag_device_view_t;

typedef struct {
    diag_slot_view_t slots[DIAG_MAX_SLOTS];
    uint8_t slot_count;
    // The devices the composition has, touch first; device_count of them.
    diag_device_view_t devices[DIAG_DEVICE_COUNT];
    uint8_t device_count;
    diag_field_view_t fields[DIAG_FIELD_COUNT];
    bool clock_set;
    char clock_text[DIAG_LINE_MAX];     // "Clock: set (rtc)", "Clock: not set"
    bool recorder_trouble;              // no medium, full, failed
    char recorder_text[DIAG_LINE_MAX];  // "Recording: on", ...

    // The network (above).
    bool network_present;    // a reporter: telemetry configured
    const char *wifi_state;  // "up", "joining", "failed", "off"; "-" without a network port
    bool wifi_trouble;       // telemetry, and the link not up
    char ssid[DIAG_SSID_MAX];
    char rssi[DIAG_SHORT_TEXT_MAX];     // "-61 dBm"; "" while not up
    char address[DIAG_SHORT_TEXT_MAX];  // "192.0.2.10"; "" while not up
    const char *broker_state;           // "online", "offline"; "off" without telemetry
    bool broker_trouble;                // telemetry, and no session
    char sent[DIAG_SHORT_TEXT_MAX];     // "sent 412", "sent 12k", "sent 999k+"; "" without
    char refused[DIAG_SHORT_TEXT_MAX];  // "refused 0" ... "refused 999+"; "" without
    bool refused_trouble;               // a refusal since start
    // "last 14:07", "last 15.10 23:58", "last --:--" without a wall clock,
    // "last -" before the first; "" without telemetry.
    char last[DIAG_SHORT_TEXT_MAX];

    // The battery: "Battery 74 %", "Battery n/a", "Battery ERR" -- the status
    // bar's number, or the home screen's word for none -- and "3976 mV"
    // whenever the gauge has a voltage, a cell's or not; "" without one.
    char battery[DIAG_SHORT_TEXT_MAX];
    bool battery_trouble;  // the gauge in error: no burst for minutes
    char battery_mv[DIAG_SHORT_TEXT_MAX];
} diag_view_t;

// What the recorder says, if there is one: on the board the card's task
// owns it, and hands this over (apps/firmware_esp32/main/card_writer.h).
typedef struct {
    bool present;
    recorder_state_t state;
} diag_recorder_t;

typedef struct {
    const air_reading_t *reading;     // required
    const sensor_manager_t *sensors;  // NULL: no sources to show
    diag_recorder_t recorder;
    // Where the wall clock came from, in the composition's word -- "rtc",
    // "console", "--wall-time" -- or NULL. Shown only while it is set.
    const char *clock_source;
    // The reporter and its network (reporter_status()), or NULL without
    // telemetry.
    const reporter_status_t *network;
    // port_clock's time at which reading->wall_time_ms was read, and the zone
    // the time of the last message is shown in; without a zone it is not.
    uint64_t now_ms;
    const time_zone_t *zone;
    // The devices beside the sensors, as the composition finds them; left
    // zero, DIAG_DEVICE_NONE: no line.
    diag_touch_t touch;
    diag_rtc_t rtc;
} diag_facts_t;

void diag_view_build(diag_view_t *v, const diag_facts_t *facts);

// Whether the screen showing `b` has nothing to redraw for `a`: everything
// either would draw is the same. Everything it compares is drawn, and
// everything drawn is compared.
bool diag_view_equal(const diag_view_t *a, const diag_view_t *b);

// "starting", "running", "failing", "restarting" for a slot.
const char *diag_view_slot_state(const sensor_manager_slot_t *slot);

// "starting", "running", "asleep", "missing", "no time" for a device; NULL
// for DIAG_DEVICE_NONE.
const char *diag_view_device_state(diag_device_state_t state);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_UI_DIAGNOSTICS_VIEW_H
