// SPDX-License-Identifier: Apache-2.0

#include "board_power.h"

#include <stdbool.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"

#include "board_config.h"

// The register's eight bits in the order they are shifted in: the first one
// pushed ends in QP7 (docs/hardware/board_notes.md, "The 74HCT4094", which
// matched the outputs against the order EPDiy's epd_board_set_ctrl() pushes
// them). The values are those epd_poweroff() leaves.
static const bool REGISTER_PUSH_ORDER[] = {
    false,  // QP7  EP_OE       output enable: off
    false,  // QP6  EP_MODE
    true,   // QP5  PWR_EN      the switched 3V3 (EPDiy's ep_scan_direction)
    false,  // QP4  EP_STV      start pulse: low
    false,  // QP3  NEG_CTRL    EPDiy's neg_power_enable; drives nothing on v2.4
    false,  // QP2  POS_CTRL    EPDiy's pos_power_enable; drives nothing on v2.4
    true,   // QP1  SMPS_CTRL   EPDiy's power_disable; drives nothing on v2.4
    false,  // QP0  EP_LE       latch enable
};

// A generous setup and hold time around each clock edge. The 74HCT4094 runs
// from VDD3V3, below the 4.5 V its HCT timing figures are given at, so
// nanoseconds are not assumed; a microsecond per edge costs nothing at boot.
#define EDGE_US 1u

static void set(int gpio, bool high)
{
    (void)gpio_set_level((gpio_num_t)gpio, high ? 1u : 0u);
    esp_rom_delay_us(EDGE_US);
}

void board_power_switched_3v3_on(const port_log_t *log)
{
    const gpio_config_t outputs = {
        .pin_bit_mask = (1ull << BOARD_EPD_CFG_DATA_GPIO) | (1ull << BOARD_EPD_CFG_CLK_GPIO) |
                        (1ull << BOARD_EPD_CFG_STR_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    // CFG_STR is GPIO0, the boot strapping pin: driving it now, after boot,
    // is safe (board_config.h).
    const esp_err_t err = gpio_config(&outputs);

    set(BOARD_EPD_CFG_STR_GPIO, false);  // hold the outputs while shifting
    for (size_t i = 0u; i < sizeof(REGISTER_PUSH_ORDER) / sizeof(REGISTER_PUSH_ORDER[0]); ++i) {
        set(BOARD_EPD_CFG_CLK_GPIO, false);
        set(BOARD_EPD_CFG_DATA_GPIO, REGISTER_PUSH_ORDER[i]);
        set(BOARD_EPD_CFG_CLK_GPIO, true);  // shifted in on the rising edge
    }
    // Latch, then close the latch again. EPDiy leaves the strobe high, which
    // keeps the storage register transparent to whatever the clock and data
    // lines do next; low, it holds what was latched until the next strobe.
    set(BOARD_EPD_CFG_STR_GPIO, true);
    set(BOARD_EPD_CFG_STR_GPIO, false);

    const log_field_t fields[] = {log_i64("gpio_config", (int64_t)err)};
    port_log_write(log, err == ESP_OK ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR, "board_power",
                   "switched 3V3 on: PWR_EN latched through the 74HCT4094, without EPDiy", fields,
                   1u);
}
