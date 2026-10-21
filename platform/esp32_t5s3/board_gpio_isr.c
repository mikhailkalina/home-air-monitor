// SPDX-License-Identifier: Apache-2.0

#include "board_gpio_isr.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"

// The service's interrupt flags (board_gpio_isr.h). Named also because
// scripts/check_sources.sh reads a bare literal as the first argument of a
// gpio_*() call as a pin number.
#define ISR_SERVICE_FLAGS 0

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_installed;

esp_err_t board_gpio_isr_service(void)
{
    taskENTER_CRITICAL(&s_lock);
    const bool installed = s_installed;
    taskEXIT_CRITICAL(&s_lock);
    if (installed) {
        return ESP_OK;
    }
    esp_err_t err = gpio_install_isr_service(ISR_SERVICE_FLAGS);
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;  // installed by someone outside these adapters
    }
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        s_installed = true;
        taskEXIT_CRITICAL(&s_lock);
    }
    return err;
}
