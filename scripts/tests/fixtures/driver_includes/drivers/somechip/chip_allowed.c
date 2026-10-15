// SPDX-License-Identifier: Apache-2.0
//
// Fixture for scripts/tests/test_driver_include_check.sh: what a driver may
// include, and near-misses of every forbidden prefix. None of it may be
// flagged. A comment naming stdio.h or esp_log.h is not an include either.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "port_i2c.h"
#include "port_sensor.h"
#include "somechip/chip_limits.h"
#include "driver_utils.h"
#include "sdl_free_name.h"
#include "stdio_like.h"
#include "espresso.h"
#include "my_freertos_shim.h"

int chip_allowed_fixture(void);
