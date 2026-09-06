// SPDX-License-Identifier: Apache-2.0
//
// Fixture for scripts/tests/test_pin_literal_check.sh.
//
// A SECOND platform's own board_config.h (not platform/esp32_t5s3/) -- this
// is the file proving the check exempts by filename pattern
// (platform/<any-name>/board_config.h), not by one hardcoded path. Every
// #define below would be flagged as a violation anywhere else.

#ifndef FIXTURE_OTHER_BOARD_CONFIG_H
#define FIXTURE_OTHER_BOARD_CONFIG_H

#define BOARD_LED_GPIO 5
#define BOARD_SENSOR_I2C_ADDRESS 0x44

#endif  // FIXTURE_OTHER_BOARD_CONFIG_H
