// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

#include "hac_test.h"
#include "ui/screen_firmware_update.h"

static void progress_measures_written_bytes_and_handles_unknown_or_extreme_sizes(void)
{
    firmware_update_progress_t p = {0};
    HAC_CHECK_EQ_INT(firmware_update_percent(&p), 0);
    p.total_bytes = 3u;
    p.written_bytes = 1u;
    HAC_CHECK_EQ_INT(firmware_update_percent(&p), 33);
    p.written_bytes = 2u;
    HAC_CHECK_EQ_INT(firmware_update_percent(&p), 66);
    p.written_bytes = 3u;
    HAC_CHECK_EQ_INT(firmware_update_percent(&p), 100);
    p.written_bytes = 4u;
    HAC_CHECK_EQ_INT(firmware_update_percent(&p), 100);
    p.total_bytes = UINT64_MAX;
    p.written_bytes = UINT64_MAX / 2u;
    HAC_CHECK_EQ_INT(firmware_update_percent(&p), 49);
    p.written_bytes = UINT64_MAX - 1u;
    HAC_CHECK_EQ_INT(firmware_update_percent(&p), 99);
    p.written_bytes = UINT64_MAX;
    HAC_CHECK_EQ_INT(firmware_update_percent(&p), 100);
    p.total_bytes = 0u;
    HAC_CHECK_EQ_INT(firmware_update_percent(&p), 0);
}

int main(void)
{
    HAC_RUN(progress_measures_written_bytes_and_handles_unknown_or_extreme_sizes);
    return hac_test_summary();
}
