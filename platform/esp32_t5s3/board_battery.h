// SPDX-License-Identifier: Apache-2.0
//
// The battery's divider on GPIO14, read through the ADC. A plain function and
// no port (docs/adr/0040-a-battery-gauge-from-the-voltage.md): one
// implementation until the simulator has a battery of its own, phase 5's
// port_power.
//
// GPIO14 is an ADC2 input, which Wi-Fi shares: adc_oneshot_read() tries ESP-IDF's
// lock and refuses at once with ESP_ERR_TIMEOUT when it is held, and a read
// itself is a conversion under a spinlock -- nothing here waits, so the event
// loop may call it between two sensors' deadlines. The board has read it 32 of
// 32 beside an idle link (docs/hardware/board_notes.md, open item on GPIO14).
//
// What it reads is the node the cell sits on, not the cell: while PWR_EN is high,
// Q3 joins that node to the LDO's input, so with USB in and no cell it reads the
// supply, and with a cell it reads the cell under the board's load
// (board_notes.md, "The battery path").

#ifndef HAC_PLATFORM_ESP32_T5S3_BOARD_BATTERY_H
#define HAC_PLATFORM_ESP32_T5S3_BOARD_BATTERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/battery_gauge.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// What one burst of reads found.
typedef struct {
    uint32_t reads;
    uint32_t ok;
    uint32_t refused;  // Wi-Fi held ADC2
    int raw_min;
    int raw_max;
    int pin_mv_min;  // calibrated, at the pin; 0 without calibration
    int pin_mv_max;
    int pin_mv_mean;
    uint32_t battery_mv;  // the node the cell sits on: the pin's mean x the divider
    uint32_t took_us;
    bool calibrated;
} board_battery_burst_t;

// The ESTIMATE of how full a cell is at a voltage, for
// core/app/battery_gauge.h: a table, the board's and not the core's.
extern const battery_curve_point_t board_battery_curve[];
extern const size_t board_battery_curve_points;

// The ADC unit, the channel and the calibration, once, before anything reads.
esp_err_t board_battery_init(void);

// Whether board_battery_init() succeeded.
bool board_battery_ready(void);

// `reads` conversions, `gap_us` apart. True when at least one succeeded and
// was calibrated, so that battery_mv means something.
bool board_battery_burst(uint32_t reads, uint32_t gap_us, board_battery_burst_t *out);

#ifdef __cplusplus
}
#endif

#endif  // HAC_PLATFORM_ESP32_T5S3_BOARD_BATTERY_H
