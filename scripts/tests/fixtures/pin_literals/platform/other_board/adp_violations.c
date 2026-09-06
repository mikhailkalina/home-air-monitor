// SPDX-License-Identifier: Apache-2.0
//
// Fixture for scripts/tests/test_pin_literal_check.sh: every construct below
// is a violation the pin-literal check (scripts/check_sources.sh, section 3)
// must report -- one per rule in pin_literal_violations().

#include "board_config.h"

// A second place claiming to define a pin, outside board_config.h.
#define STRAY_NEW_GPIO 7

void bad_gpio_direct(void)
{
    gpio_set_direction(9, 1);  // a bare pin number, not a BOARD_*_GPIO name
}

void bad_gpio_num_literal(void)
{
    gpio_set_level(GPIO_NUM_9, 1);  // ESP-IDF's own typed pin literal
}

void bad_i2c_address_call(void)
{
    i2c_master_write_to_device(0, 0x5A, 0, 0, 0);  // a bare I2C device address
}

struct fake_i2c_device_config {
    int device_address;
};

static const struct fake_i2c_device_config cfg = {
    .device_address = 0x5A,  // the new i2c_master driver's address field
};
