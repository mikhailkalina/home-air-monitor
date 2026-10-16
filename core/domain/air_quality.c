// SPDX-License-Identifier: Apache-2.0

#include "domain/air_quality.h"

#include <math.h>

iaq_level_t air_quality_iaq_level(const measurement_f32_t *iaq)
{
    if (!measurement_has_value(iaq) || !isfinite(iaq->value)) {
        return IAQ_LEVEL_UNKNOWN;
    }
    // roundf() rounds half away from zero, as the screen's formatting does
    // (view_model.c, format_fixed()).
    const float shown = roundf(iaq->value);
    if (shown <= (float)AIR_QUALITY_IAQ_EXCELLENT_MAX) {
        return IAQ_LEVEL_EXCELLENT;
    }
    if (shown <= (float)AIR_QUALITY_IAQ_GOOD_MAX) {
        return IAQ_LEVEL_GOOD;
    }
    if (shown <= (float)AIR_QUALITY_IAQ_LIGHTLY_POLLUTED_MAX) {
        return IAQ_LEVEL_LIGHTLY_POLLUTED;
    }
    if (shown <= (float)AIR_QUALITY_IAQ_MODERATELY_POLLUTED_MAX) {
        return IAQ_LEVEL_MODERATELY_POLLUTED;
    }
    return IAQ_LEVEL_POLLUTED;
}
