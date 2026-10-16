// SPDX-License-Identifier: Apache-2.0
//
// What an IAQ number means: the band it falls in
// (docs/adr/0021-measurement-accuracy-and-iaq-levels.md). In the domain, not
// in the view, because two modules need the same answer: the view model,
// which names the band under the number, and update_policy, which redraws
// when the band changes -- a band on the screen that no longer matches the
// air would be worse than none.
//
// The CO2 bands of docs/requirements.md 13.1 are older and still live in
// core/ui/view_model.h, where only the view uses them.

#ifndef HAC_CORE_DOMAIN_AIR_QUALITY_H
#define HAC_CORE_DOMAIN_AIR_QUALITY_H

#include "domain/measurement.h"

#ifdef __cplusplus
extern "C" {
#endif

// CHOICE, the owner's (2026-10-16): five bands of BSEC's 0-500 index, the
// upper bound of each inclusive. They follow the classification Bosch gives
// for its IAQ -- excellent, good, lightly polluted, moderately polluted, then
// heavily, severely and extremely polluted -- with everything above 200 merged
// into one band. That classification is not in the BSEC 2.6.1.0 archive; what
// the archive does say (bsec_datatypes.h, BSEC_OUTPUT_IAQ) is that background
// calibration scales the index so that 50 is the room's typical good air and
// 200 its typical polluted air, which these bounds agree with.
#define AIR_QUALITY_IAQ_EXCELLENT_MAX 50
#define AIR_QUALITY_IAQ_GOOD_MAX 100
#define AIR_QUALITY_IAQ_LIGHTLY_POLLUTED_MAX 150
#define AIR_QUALITY_IAQ_MODERATELY_POLLUTED_MAX 200

typedef enum {
    IAQ_LEVEL_UNKNOWN = 0,          // no number to judge
    IAQ_LEVEL_EXCELLENT,            // 0-50
    IAQ_LEVEL_GOOD,                 // 51-100
    IAQ_LEVEL_LIGHTLY_POLLUTED,     // 101-150
    IAQ_LEVEL_MODERATELY_POLLUTED,  // 151-200
    IAQ_LEVEL_POLLUTED,             // above 200
} iaq_level_t;

// The band of an IAQ measurement, or IAQ_LEVEL_UNKNOWN when it carries no
// number (measurement_has_value()). Judged on the value rounded half away
// from zero to a whole number -- what the screen shows -- so the number and
// its band never disagree: 50.4 reads "50" and is excellent, 50.5 reads "51"
// and is good. A negative index, which BSEC does not produce, is excellent.
iaq_level_t air_quality_iaq_level(const measurement_f32_t *iaq);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_DOMAIN_AIR_QUALITY_H
