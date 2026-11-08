// SPDX-License-Identifier: Apache-2.0

#include "board_battery.h"

#include <limits.h>
#include <unistd.h>

#include "board_config.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"

// ESTIMATE: a common table of a single lithium-polymer cell's resting voltage
// against its charge -- no datasheet's, and not this cell's. The board reads the
// cell under its own load, so a little low. To replace: this cell's curve from a
// discharge at the firmware's steady current, where charge left is time left
// (branch phase-5-battery-gauge, docs/hardware/phase-5-bench-plan.md).
const battery_curve_point_t board_battery_curve[] = {
    {3270u, 0u},  {3610u, 5u},  {3690u, 10u},  {3710u, 15u}, {3730u, 20u}, {3750u, 25u},
    {3770u, 30u}, {3790u, 35u}, {3800u, 40u},  {3820u, 45u}, {3840u, 50u}, {3850u, 55u},
    {3870u, 60u}, {3910u, 65u}, {3950u, 70u},  {3980u, 75u}, {4020u, 80u}, {4080u, 85u},
    {4110u, 90u}, {4150u, 95u}, {4200u, 100u},
};
const size_t board_battery_curve_points =
    sizeof(board_battery_curve) / sizeof(board_battery_curve[0]);

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_channel;
static bool s_ready;

esp_err_t board_battery_init(void)
{
    adc_unit_t unit = ADC_UNIT_1;
    esp_err_t err = adc_oneshot_io_to_channel(BOARD_BATTERY_ADC_GPIO, &unit, &s_channel);
    if (err == ESP_OK) {
        const adc_oneshot_unit_init_cfg_t unit_cfg = {
            .unit_id = unit,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    }
    if (err == ESP_OK) {
        const adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        err = adc_oneshot_config_channel(s_adc, s_channel, &chan_cfg);
    }
    if (err != ESP_OK) {
        return err;
    }
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .chan = s_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali);
    if (err != ESP_OK) {
        s_cali = NULL;  // raw reads still answer; no millivolts
    }
    s_ready = true;
    return err;
}

bool board_battery_ready(void)
{
    return s_ready;
}

bool board_battery_burst(uint32_t reads, uint32_t gap_us, board_battery_burst_t *out)
{
    *out = (board_battery_burst_t){
        .reads = reads,
        .raw_min = INT_MAX,
        .raw_max = INT_MIN,
        .pin_mv_min = INT_MAX,
        .pin_mv_max = INT_MIN,
        .calibrated = s_cali != NULL,
    };
    if (!s_ready) {
        out->refused = reads;
        return false;
    }

    const int64_t start_us = esp_timer_get_time();
    int64_t mv_sum = 0;
    uint32_t mv_count = 0u;
    for (uint32_t i = 0u; i < reads; ++i) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, s_channel, &raw) != ESP_OK) {
            out->refused++;
        } else {
            out->ok++;
            out->raw_min = (raw < out->raw_min) ? raw : out->raw_min;
            out->raw_max = (raw > out->raw_max) ? raw : out->raw_max;
            int mv = 0;
            if (s_cali != NULL && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
                out->pin_mv_min = (mv < out->pin_mv_min) ? mv : out->pin_mv_min;
                out->pin_mv_max = (mv > out->pin_mv_max) ? mv : out->pin_mv_max;
                mv_sum += mv;
                mv_count++;
            }
        }
        if (gap_us > 0u) {
            (void)usleep(gap_us);
        }
    }
    out->took_us = (uint32_t)(esp_timer_get_time() - start_us);

    if (mv_count == 0u) {
        return false;
    }
    out->pin_mv_mean = (int)(mv_sum / (int64_t)mv_count);
    out->battery_mv = (uint32_t)out->pin_mv_mean * BOARD_BATTERY_DIVIDER_RATIO;
    return true;
}
