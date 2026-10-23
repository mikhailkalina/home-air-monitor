// SPDX-License-Identifier: Apache-2.0

#include "board_button.h"

#include "driver/gpio.h"

#include "board_config.h"

esp_err_t board_button_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

bool board_button_pressed(void)
{
    return gpio_get_level((gpio_num_t)BOARD_BUTTON_GPIO) == 0;
}
