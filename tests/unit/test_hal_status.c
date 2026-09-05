// SPDX-License-Identifier: Apache-2.0
//
// hal_status_t carries one invariant that every adapter relies on when it maps
// esp_err_t, errno or a driver-specific code onto it.

#include <stddef.h>

#include "hac_test.h"
#include "hal_status.h"

static void ok_is_zero_and_every_error_is_not(void)
{
    HAC_CHECK_EQ_INT(HAL_OK, 0);

    const hal_status_t errors[] = {
        HAL_ERR_TIMEOUT, HAL_ERR_IO,          HAL_ERR_NOT_FOUND, HAL_ERR_INVALID_ARG,
        HAL_ERR_NO_MEM,  HAL_ERR_UNSUPPORTED, HAL_ERR_BUSY,      HAL_ERR_NOT_READY,
    };

    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        HAC_CHECK(errors[i] != HAL_OK);
    }
}

int main(void)
{
    HAC_RUN(ok_is_zero_and_every_error_is_not);
    return hac_test_summary();
}
