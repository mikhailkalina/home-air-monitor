// SPDX-License-Identifier: Apache-2.0
//
// The network link: on the board Wi-Fi to an access point, in the simulator a
// link a script cuts and restores (platform/host/adp_net_sim.h)
// (docs/architecture.md 5.6, docs/adr/0030-telemetry-ports-and-the-reporter.md).
//
// up() asks for the link and keeps asking: the adapter connects, and after a
// loss connects again on its own, until down(). Neither call waits -- an
// association and an address take seconds -- and state() says where it got
// to:
//
//   NET_DOWN        not asked for: never up(), or down() since
//   NET_CONNECTING  asked for, not there yet -- or lost, and being sought again
//   NET_UP          there: an address, traffic can flow
//   NET_FAILED      asked for, and the adapter has given up: something no
//                   retry fixes by itself (credentials refused, nothing
//                   configured). up() again tries again.
//
// In phase 4 the composition calls up() once, at start, and the core only
// reads state() and rssi_dbm(); phase 5's power policy will be the caller of
// down(). What uses the link -- the MQTT client behind port_telemetry --
// notices a loss on its own; this port is how the core knows why, and the
// signal strength the state message carries.
//
// The adapter wakes the event loop when the state changes, as port_input's
// does when a touch arrives: through a waker the composition gives it, which
// is the adapter's own business, not a call here. All calls are non-blocking
// and may be made from the event loop.

#ifndef HAC_PORTS_PORT_NET_H
#define HAC_PORTS_PORT_NET_H

#include <stdint.h>

#include "hal_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_DOWN = 0,
    NET_CONNECTING,
    NET_UP,
    NET_FAILED,
} net_state_t;

// What rssi_dbm() says while the link is not up.
#define PORT_NET_RSSI_NONE 0

typedef struct port_net_s port_net_t;

struct port_net_s {
    // Asks for the link, and for it to be kept: never waits. HAL_OK when the
    // request is taken -- also while already up or connecting, which changes
    // nothing. HAL_ERR_NOT_READY when the adapter cannot even begin (its
    // radio did not start); then the state is NET_FAILED.
    hal_status_t (*up)(port_net_t *self);

    // Takes the link down and stops seeking it: never waits; state() is
    // NET_DOWN once it returns. HAL_OK also when already down.
    hal_status_t (*down)(port_net_t *self);

    net_state_t (*state)(const port_net_t *self);

    // The link's signal strength in dBm, -127..-1, while state() is NET_UP;
    // PORT_NET_RSSI_NONE otherwise.
    int8_t (*rssi_dbm)(const port_net_t *self);

    void *impl;
};

// "down", "connecting", "up", "failed"; never NULL.
static inline const char *port_net_state_name(net_state_t state)
{
    switch (state) {
        case NET_DOWN:
            return "down";
        case NET_CONNECTING:
            return "connecting";
        case NET_UP:
            return "up";
        case NET_FAILED:
            return "failed";
    }
    return "?";
}

#ifdef __cplusplus
}
#endif

#endif  // HAC_PORTS_PORT_NET_H
