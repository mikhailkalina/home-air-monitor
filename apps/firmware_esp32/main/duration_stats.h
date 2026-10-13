// SPDX-License-Identifier: Apache-2.0
//
// Min, max and mean of a series of durations, for the panel calibration mode
// (epd_calibration.c, CONFIG_HAC_EPD_CALIBRATION).
//
// Pure and allocation-free on purpose, and kept out of epd_calibration.c for
// one reason: it is the only part of that mode a host test can reach.
// Everything else there drives a panel. tests/unit/test_duration_stats.c
// compiles this exact file, so the numbers that end up in
// docs/adr/0008-panel-timing.md come out of arithmetic that has been tested
// on its boundaries, not just eyeballed on a log.

#ifndef HAC_APPS_FIRMWARE_ESP32_DURATION_STATS_H
#define HAC_APPS_FIRMWARE_ESP32_DURATION_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t count;
    uint64_t min_us;  // 0 while count == 0
    uint64_t max_us;  // 0 while count == 0
    uint64_t sum_us;
} duration_stats_t;

void duration_stats_init(duration_stats_t *s);

void duration_stats_add(duration_stats_t *s, uint64_t sample_us);

// The mean, rounded to the nearest microsecond (halves round up). 0 when no
// sample has been added, which a caller distinguishes by `count`.
uint64_t duration_stats_mean_us(const duration_stats_t *s);

#ifdef __cplusplus
}
#endif

#endif  // HAC_APPS_FIRMWARE_ESP32_DURATION_STATS_H
