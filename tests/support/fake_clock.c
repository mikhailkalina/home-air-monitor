// SPDX-License-Identifier: Apache-2.0

#include "fake_clock.h"

#include <string.h>

static fake_clock_t *self_of(const port_clock_t *port)
{
    // port->impl is a non-const pointer held inside a const struct, so the
    // fake behind it stays mutable: delay_ms() has to move virtual time.
    return (fake_clock_t *)port->impl;
}

static uint64_t fc_now_ms(const port_clock_t *port)
{
    return self_of(port)->now_ms;
}

static uint64_t fc_wall_ms(const port_clock_t *port)
{
    const fake_clock_t *fc = self_of(port);

    if (!fc->wall_synced) {
        return 0u;  // the port contract: 0 means "not synchronized"
    }
    return fc->now_ms + fc->wall_offset_ms;
}

static void fc_delay_ms(const port_clock_t *port, uint32_t ms)
{
    fake_clock_t *fc = self_of(port);

    fc->delay_calls++;
    fc->delayed_total_ms += ms;
    fc->now_ms += ms;
}

void fake_clock_init(fake_clock_t *fc)
{
    memset(fc, 0, sizeof(*fc));

    fc->port.now_ms = fc_now_ms;
    fc->port.wall_ms = fc_wall_ms;
    fc->port.delay_ms = fc_delay_ms;
    fc->port.impl = fc;
}

const port_clock_t *fake_clock_port(const fake_clock_t *fc)
{
    return &fc->port;
}

void fake_clock_advance(fake_clock_t *fc, uint64_t ms)
{
    fc->now_ms += ms;
}

void fake_clock_set_wall(fake_clock_t *fc, uint64_t wall_ms)
{
    // Unsigned wraparound is well defined and cancels out in fc_wall_ms(),
    // so a wall clock behind the monotonic one round-trips correctly.
    fc->wall_offset_ms = wall_ms - fc->now_ms;
    fc->wall_synced = true;
}
