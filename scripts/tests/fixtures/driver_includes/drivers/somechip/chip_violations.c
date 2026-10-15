// SPDX-License-Identifier: Apache-2.0
//
// Fixture for scripts/tests/test_driver_include_check.sh: one include per
// rule of driver_include_violations(), each of which must be flagged.

#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "driver/i2c_master.h"
#include <SDL2/SDL.h>
# include <stdio.h>

int chip_violations_fixture(void);
