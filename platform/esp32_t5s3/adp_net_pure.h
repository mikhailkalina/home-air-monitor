// SPDX-License-Identifier: Apache-2.0
//
// The decisions platform/esp32_t5s3/adp_net.c makes about ESP-IDF's Wi-Fi
// station, kept free of ESP-IDF headers so the host build can test them
// (tests/unit/test_adp_net_pure.c), as adp_i2c_pure.c is
// (docs/adr/0032-wifi-on-the-board.md). The adapter feeds this machine what
// the callers ask and what the driver reports, does what it answers, and
// shows port_net the state it gives. Everything that touches the driver stays
// in adp_net.c.
//
// The machine's phases, and the port_net state each is:
//
//   IDLE      not asked for                                   NET_DOWN
//   STARTING  asked for: the radio starting                   NET_CONNECTING
//   JOINING   an attempt: scan, association, handshake, DHCP  NET_CONNECTING
//   UP        an address                                      NET_UP
//   WAITING   between attempts: the backoff                   NET_CONNECTING
//   FAILED    the radio would not start: given up until up()  NET_FAILED
//
// A link that was up and is lost is sought again at once; an attempt that
// fails waits the backoff (adp_net_limits.h), which doubles while attempts
// keep failing and starts over once the link is up. Credentials the network
// refuses are retried as any other failure, at the backoff's ceiling, and
// never make the machine give up: after a power cut the access point and the
// device come up together, and an access point still starting may refuse a
// handshake it accepts a minute later -- a device that gave up then would
// stay offline until someone restarted it. The refusals are counted, for the
// console and the log to say "the password?".
//
// Events that belong to no attempt under way -- an address arriving after
// down(), a disconnection after the adapter gave an attempt up -- are
// ignored: the driver's events reach the adapter in the order the driver
// posted them, and an attempt starts only after the radio's own start event,
// so anything earlier is behind it.

#ifndef HAC_PLATFORM_ESP32_T5S3_ADP_NET_PURE_H
#define HAC_PLATFORM_ESP32_T5S3_ADP_NET_PURE_H

#include <stdbool.h>
#include <stdint.h>

#include "adp_net_limits.h"
#include "port_net.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADP_NET_NO_DEADLINE UINT64_MAX

// The disconnection reasons this adapter names: 802.11's and ESP-IDF's own
// (wifi_err_reason_t, esp_wifi_types_generic.h in ESP-IDF 5.5), which
// adp_net.c checks against ESP-IDF's enum at compile time. The event carries
// a byte.
#define ADP_NET_REASON_UNSPECIFIED 1u
#define ADP_NET_REASON_AUTH_EXPIRE 2u
#define ADP_NET_REASON_AUTH_LEAVE 3u
#define ADP_NET_REASON_DISASSOC_DUE_TO_INACTIVITY 4u
#define ADP_NET_REASON_ASSOC_TOOMANY 5u
#define ADP_NET_REASON_ASSOC_LEAVE 8u
#define ADP_NET_REASON_MIC_FAILURE 14u
#define ADP_NET_REASON_4WAY_HANDSHAKE_TIMEOUT 15u
#define ADP_NET_REASON_802_1X_AUTH_FAILED 23u
#define ADP_NET_REASON_BEACON_TIMEOUT 200u
#define ADP_NET_REASON_NO_AP_FOUND 201u
#define ADP_NET_REASON_AUTH_FAIL 202u
#define ADP_NET_REASON_ASSOC_FAIL 203u
#define ADP_NET_REASON_HANDSHAKE_TIMEOUT 204u
#define ADP_NET_REASON_CONNECTION_FAIL 205u
#define ADP_NET_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY 210u
#define ADP_NET_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD 211u
#define ADP_NET_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD 212u

// Not the driver's: what ended an attempt the adapter itself ended. Above
// any byte the driver can report.
#define ADP_NET_REASON_NONE 0u               // nothing has ended an attempt yet
#define ADP_NET_REASON_ATTEMPT_TIMEOUT 256u  // no address within the timeout
#define ADP_NET_REASON_CONNECT_ERROR 257u    // esp_wifi_connect() refused to begin

typedef enum {
    ADP_NET_PHASE_IDLE = 0,
    ADP_NET_PHASE_STARTING,
    ADP_NET_PHASE_JOINING,
    ADP_NET_PHASE_UP,
    ADP_NET_PHASE_WAITING,
    ADP_NET_PHASE_FAILED,
} adp_net_phase_t;

// What the adapter is to do with the driver.
typedef enum {
    ADP_NET_ACT_NONE = 0,
    ADP_NET_ACT_START,       // esp_wifi_start(): its error, or STA_START, follows
    ADP_NET_ACT_CONNECT,     // esp_wifi_connect(): an attempt
    ADP_NET_ACT_DISCONNECT,  // esp_wifi_disconnect(): the attempt is given up
    ADP_NET_ACT_STOP,        // esp_wifi_stop()
} adp_net_action_t;

typedef struct {
    uint32_t backoff_first_ms;
    uint32_t backoff_max_ms;
    uint32_t attempt_timeout_ms;
} adp_net_machine_config_t;

// Since adp_net_machine_init(), for the console and the log.
typedef struct {
    uint32_t starts;             // the radio started
    uint32_t attempts;           // esp_wifi_connect() asked
    uint32_t attempts_failed;    // of those, ended without an address
    uint32_t timeouts;           // of those, given up by the adapter
    uint32_t connections;        // an address reached
    uint32_t losses;             // a link that was up, lost
    uint32_t failures_in_a_row;  // attempts failed since the link was last up, or since up()
    uint32_t refusals_in_a_row;  // of those, the ones adp_net_reason_is_refusal() names
    uint16_t last_reason;        // what ended the last attempt or link; ADP_NET_REASON_NONE
    uint64_t up_since_ms;        // UP: since when
    uint64_t sought_since_ms;    // asked for and not up: since up(), or since the loss
} adp_net_machine_stats_t;

typedef struct {
    adp_net_machine_config_t cfg;
    adp_net_phase_t phase;
    bool radio_on;         // started, or being started, and not stopped since
    bool associated;       // JOINING: the attempt has associated
    uint64_t deadline_ms;  // STARTING, JOINING: given up at; WAITING: the next attempt
    uint32_t backoff_ms;   // the wait after the next failure
    adp_net_machine_stats_t stats;
} adp_net_machine_t;

// ADP_NET_* of adp_net_limits.h.
adp_net_machine_config_t adp_net_machine_config_default(void);

// IDLE, the radio off.
void adp_net_machine_init(adp_net_machine_t *m, const adp_net_machine_config_t *cfg);

// The callers: up() and down() as the port takes them.
adp_net_action_t adp_net_machine_up(adp_net_machine_t *m, uint64_t now_ms);
adp_net_action_t adp_net_machine_down(adp_net_machine_t *m, uint64_t now_ms);

// The radio's start: `ok` false for esp_wifi_start()'s error, true for its
// STA_START event.
adp_net_action_t adp_net_machine_started(adp_net_machine_t *m, bool ok, uint64_t now_ms);

// esp_wifi_connect() refused to begin the attempt it was asked for.
adp_net_action_t adp_net_machine_connect_error(adp_net_machine_t *m, uint64_t now_ms);

// STA_CONNECTED: the attempt has associated; an address is next.
void adp_net_machine_associated(adp_net_machine_t *m, uint64_t now_ms);

// IP_EVENT_STA_GOT_IP, and IP_EVENT_STA_LOST_IP.
adp_net_action_t adp_net_machine_got_address(adp_net_machine_t *m, uint64_t now_ms);
adp_net_action_t adp_net_machine_lost_address(adp_net_machine_t *m, uint64_t now_ms);

// STA_DISCONNECTED, with its reason.
adp_net_action_t adp_net_machine_disconnected(adp_net_machine_t *m, uint16_t reason,
                                              uint64_t now_ms);

// Time passing: the next attempt when the backoff is over, an attempt or a
// start given up at its deadline.
adp_net_action_t adp_net_machine_tick(adp_net_machine_t *m, uint64_t now_ms);

// When tick() next has something to do, or ADP_NET_NO_DEADLINE.
uint64_t adp_net_machine_next_ms(const adp_net_machine_t *m);

net_state_t adp_net_machine_state(const adp_net_machine_t *m);

// "idle", "starting", "joining", "up", "waiting", "failed"; never NULL.
const char *adp_net_phase_name(adp_net_phase_t phase);

// A short name for a reason -- "no_ap_found", "auth_fail", "attempt_timeout"
// -- or "other" for one this adapter does not name; never NULL.
const char *adp_net_reason_name(uint16_t reason);

// Whether `reason` is the network refusing the credentials, or a handshake
// that fails as a wrong password makes it fail: a WPA2 network refuses a wrong
// password with a 4-way handshake that times out, not with a word of its own.
bool adp_net_reason_is_refusal(uint16_t reason);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_ADP_NET_PURE_H
