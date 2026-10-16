// SPDX-License-Identifier: Apache-2.0

#include "adp_clock.h"

#include <sys/time.h>
#include <sys/types.h>
#include <time.h>

#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint64_t clk_now_ms(const port_clock_t *port)
{
    (void)port;
    return (uint64_t)(esp_timer_get_time() / 1000);
}

#define MS_PER_S 1000u
#define US_PER_MS 1000u

static uint64_t clk_wall_ms(const port_clock_t *port)
{
    (void)port;
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0 || tv.tv_sec < 0) {
        return 0u;
    }
    const uint64_t ms = (uint64_t)tv.tv_sec * MS_PER_S + (uint64_t)tv.tv_usec / US_PER_MS;
    // The port contract: 0 means "not synchronized" -- a system time nobody
    // set (adp_clock.h).
    return (ms < ADP_CLOCK_WALL_EARLIEST_MS) ? 0u : ms;
}

static void clk_delay_ms(const port_clock_t *port, uint32_t ms)
{
    const adp_clock_t *c = (const adp_clock_t *)port->impl;

    if (ms == 0u) {
        return;
    }

    const int64_t start_us = esp_timer_get_time();

    if (pdMS_TO_TICKS(ms) == 0u) {
        // Shorter than one tick period: rounding down to vTaskDelay(0) would
        // not sleep at all, which breaks the port's "advances by at least
        // this many ms" contract. Busy-wait instead; ms is small here by
        // construction (less than one tick, typically 1-10 ms), so the
        // multiplication cannot overflow.
        esp_rom_delay_us(ms * 1000u);
    } else {
        // pdMS_TO_TICKS() rounds DOWN, so a naive vTaskDelay(pdMS_TO_TICKS(ms))
        // can already sleep for less than `ms` -- delay_ms(15) at the default
        // 100 Hz tick rate asks for 1.5 ticks, pdMS_TO_TICKS() truncates that
        // to 1, and 1 tick is only 10 ms. Rounding up to whole ticks fixes
        // that, but not the rest of it: vTaskDelay(N) itself only guarantees
        // MORE than (N-1) tick periods have elapsed, not N, because it wakes
        // when the tick count reaches a target computed from whatever tick
        // count was already current when this call happened to land -- which
        // can be anywhere within the tick already under way, from just
        // started to just about to roll over. In the worst case, up to a
        // full tick period is "free" and never counted. See
        // docs/adr/0006-delay-ms-rounds-up-a-full-extra-tick.md for the
        // derivation and why this is not specific to small `ms`: rounding up
        // to whole ticks alone under-delivers by up to one tick period
        // regardless of how large `ms` is, and is worst, proportionally, at
        // delay_ms(1) -- where a whole tick's worth of rounding still is not
        // enough to trust on its own.
        //
        // Both gaps close with one extra tick: ceil(ms / tick_period) covers
        // the truncation, and unconditionally adding 1 more tick turns the
        // guaranteed minimum, (ticks - 1) * tick_period, into
        // ceil(ms / tick_period) * tick_period, which is never less than ms.
        const uint32_t hz = (uint32_t)configTICK_RATE_HZ;
        const TickType_t ceil_ticks = (TickType_t)(((uint64_t)ms * hz + 999u) / 1000u);
        vTaskDelay(ceil_ticks + 1u);
    }

    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    const log_field_t fields[] = {
        log_u64("requested_ms", ms),
        log_i64("elapsed_us", elapsed_us),
    };
    port_log_write(c->log, LOG_LEVEL_DEBUG, "adp_clock", "delay_ms", fields,
                   sizeof(fields) / sizeof(fields[0]));
}

void adp_clock_init(adp_clock_t *c, const port_log_t *log)
{
    c->log = log;
    c->port.now_ms = clk_now_ms;
    c->port.wall_ms = clk_wall_ms;
    c->port.delay_ms = clk_delay_ms;
    c->port.impl = c;
}

const port_clock_t *adp_clock_port(const adp_clock_t *c)
{
    return &c->port;
}

hal_status_t adp_clock_set_wall_ms(adp_clock_t *c, uint64_t unix_ms)
{
    if (unix_ms < ADP_CLOCK_WALL_EARLIEST_MS) {
        return HAL_ERR_INVALID_ARG;
    }
    const struct timeval tv = {
        .tv_sec = (time_t)(unix_ms / MS_PER_S),
        .tv_usec = (suseconds_t)((unix_ms % MS_PER_S) * US_PER_MS),
    };
    const int rc = settimeofday(&tv, NULL);
    const log_field_t fields[] = {
        log_u64("unix_ms", unix_ms),
        log_i64("rc", (int64_t)rc),
    };
    port_log_write(c->log, rc == 0 ? LOG_LEVEL_DEBUG : LOG_LEVEL_ERROR, "adp_clock",
                   "system time set", fields, sizeof(fields) / sizeof(fields[0]));
    return (rc == 0) ? HAL_OK : HAL_ERR_IO;
}
