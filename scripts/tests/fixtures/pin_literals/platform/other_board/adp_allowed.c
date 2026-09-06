// SPDX-License-Identifier: Apache-2.0
//
// Fixture for scripts/tests/test_pin_literal_check.sh: every construct below
// is the intended reuse pattern, or a deliberate near-miss of a violation
// pattern, and the pin-literal check must stay silent on all of it.

#include "board_config.h"

void good_gpio_reuse(void)
{
    // A named constant, not a literal -- BOARD_LED_GPIO cannot start with a
    // digit, so the gpio_*() rule cannot mistake this for a violation.
    gpio_set_direction(BOARD_LED_GPIO, 1);
}

void good_i2c_data_byte(void)
{
    // i2c_master_write_byte()'s second argument is an ordinary data byte
    // written to the bus, never a device address -- unlike
    // i2c_master_write_to_device() and friends, this must NOT be flagged, or
    // the vast majority of real I2C driver code would trip the check.
    i2c_master_write_byte(0 /* cmd handle */, 0x5A, 1 /* ack_en */);
}
