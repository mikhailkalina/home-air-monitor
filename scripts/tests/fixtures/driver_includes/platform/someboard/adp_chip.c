// SPDX-License-Identifier: Apache-2.0
//
// Fixture for scripts/tests/test_driver_include_check.sh: a platform adapter
// is where ESP-IDF belongs. Not a driver, so never flagged by this check.

#include <stdio.h>
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
