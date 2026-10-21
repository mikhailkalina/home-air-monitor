// SPDX-License-Identifier: Apache-2.0

#include "board_rtc_int.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "board_config.h"
#include "board_gpio_isr.h"

#define TAG "board_rtc_int"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static board_rtc_int_edge_t s_edge;

static void IRAM_ATTR on_edge(void *arg)
{
    (void)arg;
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_lock);
    s_edge.count++;
    s_edge.at_us = (uint64_t)now_us;
    portEXIT_CRITICAL_ISR(&s_lock);
}

hal_status_t board_rtc_int_start(const port_log_t *log)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ull << BOARD_RTC_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err == ESP_OK) {
        err = board_gpio_isr_service();  // installed once for every adapter on it
    }
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(BOARD_RTC_INT_GPIO, on_edge, NULL);
    }
    const log_field_t fields[] = {
        log_u64("gpio", BOARD_RTC_INT_GPIO),
        log_str("level", board_rtc_int_low() ? "low" : "high"),
        log_i64("esp_err", (int64_t)err),
    };
    port_log_write(log, (err == ESP_OK) ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR, TAG,
                   (err == ESP_OK) ? "the RTC's INT interrupting on its falling edge"
                                   : "the RTC's INT not interrupting: no tick to measure",
                   fields, sizeof(fields) / sizeof(fields[0]));
    return (err == ESP_OK) ? HAL_OK : HAL_ERR_IO;
}

void board_rtc_int_edge(board_rtc_int_edge_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_edge;
    portEXIT_CRITICAL(&s_lock);
}

bool board_rtc_int_low(void)
{
    return gpio_get_level(BOARD_RTC_INT_GPIO) == 0;
}
