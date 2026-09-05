// SPDX-License-Identifier: Apache-2.0

#include "adp_clock_virtual.h"

#include <string.h>

static adp_clock_virtual_t *self_of(const port_clock_t *port)
{
    // port->impl is a non-const pointer held inside a const struct, so the
    // adapter behind it stays mutable: delay_ms() has to move virtual time.
    return (adp_clock_virtual_t *)port->impl;
}

static uint64_t vc_now_ms(const port_clock_t *port)
{
    return self_of(port)->now_ms;
}

static uint64_t vc_wall_ms(const port_clock_t *port)
{
    const adp_clock_virtual_t *vc = self_of(port);

    if (!vc->wall_synced) {
        return 0u;  // the port contract: 0 means "not synchronized"
    }
    return vc->now_ms + vc->wall_offset_ms;
}

static void vc_delay_ms(const port_clock_t *port, uint32_t ms)
{
    adp_clock_virtual_t *vc = self_of(port);

    vc->delay_calls++;
    vc->delayed_total_ms += ms;
    vc->now_ms += ms;
}

void adp_clock_virtual_init(adp_clock_virtual_t *vc, double time_scale)
{
    memset(vc, 0, sizeof(*vc));

    vc->port.now_ms = vc_now_ms;
    vc->port.wall_ms = vc_wall_ms;
    vc->port.delay_ms = vc_delay_ms;
    vc->port.impl = vc;

    vc->time_scale = (time_scale > 0.0) ? time_scale : 1.0;
}

const port_clock_t *adp_clock_virtual_port(const adp_clock_virtual_t *vc)
{
    return &vc->port;
}

void adp_clock_virtual_advance(adp_clock_virtual_t *vc, uint64_t virtual_ms)
{
    vc->now_ms += virtual_ms;
}

uint64_t adp_clock_virtual_scale_ms(const adp_clock_virtual_t *vc, uint64_t real_ms)
{
    return (uint64_t)((double)real_ms * vc->time_scale);
}

void adp_clock_virtual_set_scale(adp_clock_virtual_t *vc, double time_scale)
{
    vc->time_scale = (time_scale > 0.0) ? time_scale : 1.0;
}

double adp_clock_virtual_scale(const adp_clock_virtual_t *vc)
{
    return vc->time_scale;
}

void adp_clock_virtual_set_wall(adp_clock_virtual_t *vc, uint64_t wall_ms)
{
    // Unsigned wraparound is well defined and cancels out in vc_wall_ms(), so
    // a wall clock behind the monotonic one round-trips correctly.
    vc->wall_offset_ms = wall_ms - vc->now_ms;
    vc->wall_synced = true;
}
