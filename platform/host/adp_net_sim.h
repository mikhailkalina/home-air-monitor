// SPDX-License-Identifier: Apache-2.0
//
// port_net over a simulated access point, for the simulator and the tests
// (docs/adr/0030-telemetry-ports-and-the-reporter.md). Time is the virtual
// clock's: up() starts an association that completes ADP_NET_SIM_CONNECT_MS
// later; the link the access point gives can be cut and restored -- now, by
// adp_net_sim_set_link(), or at set times, by a script -- and while up() is
// in force a restored link is associated again, as a Wi-Fi manager does.
//
// The command line's form, parsed by adp_net_sim_add_script():
//
//   --net-script "<t_s>:down;<t_s>:up;..."
//
// at t_s seconds (a decimal) the access point goes away, or comes back, in
// time order. The signal strength is fixed (ADP_NET_SIM_DEFAULT_RSSI_DBM),
// and so are the network's name and the address the link gives
// (ADP_NET_SIM_DEFAULT_SSID, ADP_NET_SIM_ADDRESS_*), which info() reports as
// the board's Wi-Fi will (docs/adr/0031-the-network-on-the-screen.md);
// scenarios/wifi_flapping.csv, a profile with RSSI, is not read yet.
//
// A listener, when set, hears each change of the link with its own time --
// how the simulator's loopback client loses its connection when the network
// under it goes (adp_telemetry_loopback.h).

#ifndef HAC_PLATFORM_HOST_ADP_NET_SIM_H
#define HAC_PLATFORM_HOST_ADP_NET_SIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"
#include "port_clock.h"
#include "port_net.h"

#ifdef __cplusplus
extern "C" {
#endif

// Room for a link that flaps every few seconds for half an hour
// (tests/cli/simulator_link_flapping.cmake).
#define ADP_NET_SIM_MAX_EVENTS 1024u

// ESTIMATE: an association and an address -- Wi-Fi's handshakes and DHCP on
// a home network take one to a few seconds. Nothing has measured the
// board's; phase 4c's Wi-Fi adapter does.
#define ADP_NET_SIM_CONNECT_MS 3000u

// CHOICE: the signal the simulated link reports: a good one, as
// mqtt_messages_dump.c's.
#define ADP_NET_SIM_DEFAULT_RSSI_DBM (-61)

// CHOICES: the network's name, and the address the link gives -- 192.0.2.10,
// in TEST-NET-1, the block RFC 5737 sets aside for documentation, so it is
// never mistaken for a real host on someone's network.
#define ADP_NET_SIM_DEFAULT_SSID "sim-ap"
#define ADP_NET_SIM_ADDRESS_0 192u
#define ADP_NET_SIM_ADDRESS_1 0u
#define ADP_NET_SIM_ADDRESS_2 2u
#define ADP_NET_SIM_ADDRESS_3 10u

#define ADP_NET_SIM_NO_EVENT UINT64_MAX

// Hears the link go (`present` false) or come back, at `at_ms`.
typedef void (*adp_net_sim_listener_fn)(void *ctx, bool present, uint64_t at_ms);

typedef struct {
    uint64_t at_ms;
    bool present;
} adp_net_sim_event_t;

typedef struct {
    port_net_t port;
    const port_clock_t *clock;
    int8_t rssi_dbm;
    uint32_t connect_ms;

    bool wanted;                // up() in force
    bool link;                  // the access point is there
    bool associating;           // wanted and linked, not up yet
    uint64_t associated_at_ms;  // when the association completes

    adp_net_sim_event_t events[ADP_NET_SIM_MAX_EVENTS];
    size_t count;
    size_t next;

    adp_net_sim_listener_fn listener;
    void *listener_ctx;

    // Counters, for tests and the simulator's summary.
    uint32_t ups;
    uint32_t downs;
    uint32_t links_lost;
} adp_net_sim_t;

// Down, the access point there, no script. `clock` must outlive it.
void adp_net_sim_init(adp_net_sim_t *n, const port_clock_t *clock);

port_net_t *adp_net_sim_port(adp_net_sim_t *n);

void adp_net_sim_set_listener(adp_net_sim_t *n, adp_net_sim_listener_fn fn, void *ctx);

// The access point goes away (false) or comes back, now.
void adp_net_sim_set_link(adp_net_sim_t *n, bool present);

// Parses `text` (above) and adds its changes. HAL_ERR_INVALID_ARG, with a
// sentence in `err` saying what is wrong and nothing added, for a malformed
// item, a time out of order or earlier than what the script holds, or more
// than fit.
hal_status_t adp_net_sim_add_script(adp_net_sim_t *n, const char *text, char *err, size_t err_cap);

// When the state next changes by itself -- a scripted change, an
// association completing -- or ADP_NET_SIM_NO_EVENT. The loop wakes then.
uint64_t adp_net_sim_next_ms(const adp_net_sim_t *n);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_HOST_ADP_NET_SIM_H
