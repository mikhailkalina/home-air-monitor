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
//   * the recorder's state.
//
// Nothing on it changes with time alone, and nothing that changes at every
// poll -- no read counts, no ages, no uptime: on e-paper a redraw costs a
// partial refresh of the budget, and the screen redraws only when what it
// says has changed (diag_view_equal()). The slot states fold IDLE and
// MEASURING into "running" for the same reason: a forced-mode BME68x passes
// through both every cycle.
//
// The network's words are the exception, and say so: the signal, the counts
// and the time of the last message change in ordinary operation -- every
// minute at least -- and the states with every reconnection attempt, so they
// are drawn as they are when the screen is drawn and redraw nothing by
// themselves. What redraws the section is the link the status bar shows,
// held as the bar holds it (core/app/link_hold.h) -- and drawn here too, the
// bar's own word in the title line, so that its change is always a change of
// the picture -- and the network's name: a flapping link costs this screen
// nothing either. (Compared and not drawn, a change of the held link made a
// redraw due whose picture was the one on the glass: an empty diff, tried
// again every second until the screen timed out.)

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
    diag_slot_view_t slots[DIAG_MAX_SLOTS];
    uint8_t slot_count;
    diag_field_view_t fields[DIAG_FIELD_COUNT];
    bool clock_set;
    char clock_text[DIAG_LINE_MAX];     // "Clock: set (rtc)", "Clock: not set"
    bool recorder_trouble;              // no medium, full, failed
    char recorder_text[DIAG_LINE_MAX];  // "Recording: on", ...

    // The network (above). What redraws: whether there is telemetry, the
    // held link and the network's name. The rest is as of the drawing.
    bool network_present;    // a reporter: telemetry configured
    link_status_t link;      // the held link, the status bar's
    const char *link_text;   // and its word, view_model_link_text(): "" before one has held
    bool link_trouble;       // no Wi-Fi, or no broker
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
    // telemetry. The held link is the reading's.
    const reporter_status_t *network;
    // port_clock's time at which reading->wall_time_ms was read, and the zone
    // the time of the last message is shown in; without a zone it is not.
    uint64_t now_ms;
    const time_zone_t *zone;
} diag_facts_t;

void diag_view_build(diag_view_t *v, const diag_facts_t *facts);

// Whether the screen showing `b` has nothing to redraw for `a`: everything
// either would draw is the same but the network's words that are as of the
// drawing (above). Everything it compares is drawn.
bool diag_view_equal(const diag_view_t *a, const diag_view_t *b);

// "starting", "running", "failing", "restarting" for a slot.
const char *diag_view_slot_state(const sensor_manager_slot_t *slot);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_UI_DIAGNOSTICS_VIEW_H
