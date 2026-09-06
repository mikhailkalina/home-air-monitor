// SPDX-License-Identifier: Apache-2.0

#include "adp_clock.h"

#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint64_t clk_now_ms(const port_clock_t *port)
{
    (void)port;
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static uint64_t clk_wall_ms(const port_clock_t *port)
{
    (void)port;
    return 0u;  // the port contract: 0 means "not synchronized" -- true until phase 3
}

static void clk_delay_ms(const port_clock_t *port, uint32_t ms)
{
    (void)port;

    if (ms == 0u) {
        return;
    }

    const TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ticks == 0u) {
        // Shorter than one tick period: rounding down to vTaskDelay(0) would
        // not sleep at all, which breaks the port's "advances by at least
        // this many ms" contract. Busy-wait instead; ms is small here by
        // construction (less than one tick, typically 1-10 ms), so the
        // multiplication cannot overflow.
        esp_rom_delay_us(ms * 1000u);
        return;
    }
    vTaskDelay(ticks);
}

void adp_clock_init(adp_clock_t *c)
{
    c->port.now_ms = clk_now_ms;
    c->port.wall_ms = clk_wall_ms;
    c->port.delay_ms = clk_delay_ms;
    c->port.impl = c;
}

const port_clock_t *adp_clock_port(const adp_clock_t *c)
{
    return &c->port;
}
