// SPDX-License-Identifier: Apache-2.0
//
// port_net on the board: ESP-IDF's Wi-Fi station (phase 4e,
// docs/adr/0032-wifi-on-the-board.md). Everything that decides anything --
// when to try again, what gives an attempt up, which events belong to the
// attempt under way -- is adp_net_pure.c, which the host tests. This adds
// what is the board's:
//
//   * ESP-IDF's station, set up once by adp_net_init(): esp_netif, the
//     default event loop, the driver -- its memory taken, its task running,
//     the radio still off. The credentials go to the driver in RAM only:
//     neither the driver's NVS copy nor its storage is used, so an image
//     built without them finds none left behind in flash by an earlier one.
//     The flash's NVS partition is opened, never erased, for the radio's
//     calibration data alone;
//   * a task of its own, which owns the machine and the driver: up() and
//     down() only record what is asked and wake it, and the driver's events,
//     posted by ESP-IDF's event task, wait for it in a queue. It starts the
//     radio -- the RF calibration's current, which is why the composition
//     calls up() only once the first frame is on the glass -- connects,
//     backs off, gives up an attempt at its timeout, reads the signal
//     strength every ADP_NET_RSSI_REFRESH_MS, and wakes the event loop
//     through the waker the composition gives it whenever the state the port
//     shows changes;
//   * what the port shows, under a spinlock the task holds only to copy a few
//     bytes: state(), rssi_dbm() and info() never wait. Until the task has
//     acted on an up() or a down(), the port shows what was asked --
//     NET_CONNECTING, NET_DOWN -- so that neither is ever contradicted by a
//     state from before it.
//
// One station: ESP-IDF's driver is a single instance, and so is this.

#ifndef HAC_PLATFORM_ESP32_T5S3_ADP_NET_H
#define HAC_PLATFORM_ESP32_T5S3_ADP_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "adp_net_limits.h"
#include "adp_net_pure.h"
#include "hal_status.h"
#include "port_clock.h"
#include "port_log.h"
#include "port_net.h"

#ifdef __cplusplus
extern "C" {
#endif

// The six bytes of a MAC address.
#define ADP_NET_MAC_LEN 6u

// The longest password WPA2-PSK takes: 63 characters, or 64 hex digits.
#define ADP_NET_PASSWORD_MAX 64u
// The shortest WPA2-PSK takes; an empty one is an open network.
#define ADP_NET_PASSWORD_MIN 8u

// Called from the adapter's task when the state the port shows changed.
typedef void (*adp_net_waker_fn)(void *ctx);

typedef struct {
    const char *ssid;      // 1..PORT_NET_SSID_MAX octets
    const char *password;  // "" for an open network, else 8..64 characters; never logged
    const char *hostname;  // what DHCP tells the router; NULL keeps ESP-IDF's
    const port_clock_t *clock;
    const port_log_t *log;
    uint32_t task_stack_bytes;
    int task_priority;
    int task_core;
} adp_net_config_t;

// What the console's `wifi` prints, as the adapter's task last left it.
typedef struct {
    hal_status_t init_status;
    net_state_t state;  // what state() says
    adp_net_phase_t phase;
    char ssid[PORT_NET_SSID_MAX + 1u];
    uint8_t mac[ADP_NET_MAC_LEN];
    uint8_t ipv4[PORT_NET_IPV4_LEN];     // while up
    uint8_t gateway[PORT_NET_IPV4_LEN];  // while up
    int8_t rssi_dbm;                     // while up, else PORT_NET_RSSI_NONE
    uint8_t channel;                     // the last association's
    uint8_t bssid[ADP_NET_MAC_LEN];      // the last association's
    adp_net_machine_stats_t stats;
    uint64_t next_attempt_ms;  // WAITING: when, on port_clock; else ADP_NET_NO_DEADLINE
    uint32_t events_dropped;
    uint32_t stack_free_min_bytes;
} adp_net_status_t;

typedef struct {
    port_net_t port;
    adp_net_config_t cfg;
    char ssid[PORT_NET_SSID_MAX + 1u];
    uint8_t mac[ADP_NET_MAC_LEN];
    hal_status_t init_status;

    void *queue;         // QueueHandle_t of the task's messages
    void *task;          // TaskHandle_t
    void *netif;         // esp_netif_t *
    void *wifi_handler;  // esp_event_handler_instance_t
    void *ip_handler;

    adp_net_waker_fn waker;
    void *waker_ctx;

    // The task's alone.
    adp_net_machine_t machine;
    uint64_t rssi_due_ms;
    uint16_t logged_reason;  // the last failure reason logged at INFO
    net_state_t woken_for;   // the state the waker was last called for

    // Between the callers, the event task and the adapter's task, under the
    // adapter's spinlock.
    bool wanted;        // up() in force
    uint32_t requests;  // up() and down() calls so far
    uint32_t served;    // ... the task has acted on
    adp_net_status_t shown;
} adp_net_t;

// Sets up ESP-IDF's station for `cfg->ssid` and starts the adapter's task;
// the radio stays off until up(). Returns at once. Anything that fails is
// logged and leaves the port answering -- down, and NET_FAILED with
// HAL_ERR_NOT_READY at up() -- with its status here: HAL_ERR_INVALID_ARG for
// a name or a password the station cannot take, HAL_ERR_NO_MEM or HAL_ERR_IO
// for ESP-IDF's own refusals. Call once.
hal_status_t adp_net_init(adp_net_t *a, const adp_net_config_t *cfg);

port_net_t *adp_net_port(adp_net_t *a);

// Who the task wakes when the state the port shows changes. May be set at any
// time; set it before up() to hear every change.
void adp_net_set_waker(adp_net_t *a, adp_net_waker_fn fn, void *ctx);

// The station's own MAC address, from the chip's eFuses: needs no Wi-Fi, so
// the composition can name the device before the adapter exists.
hal_status_t adp_net_station_mac(uint8_t out[ADP_NET_MAC_LEN]);

void adp_net_status(adp_net_t *a, adp_net_status_t *out);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_ADP_NET_H
