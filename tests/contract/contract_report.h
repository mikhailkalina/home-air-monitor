// SPDX-License-Identifier: Apache-2.0
//
// The reporting seam every contract body in this directory uses, so that one
// body can run under two very different runners: a host CTest binary that
// prints through hac_test.h, and the ESP32 firmware, which has neither.
// Keeping the typedef in one header (rather than redeclaring it per contract)
// is what lets tests/contract/CMakeLists.txt and
// apps/firmware_esp32/components/hac_contract/CMakeLists.txt compile the
// same .c files against two different reporters without either copying the
// other's declaration out of step.

#ifndef HAC_TESTS_CONTRACT_REPORT_H
#define HAC_TESTS_CONTRACT_REPORT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called once per check. `check` is a stable, allocation-free description of
// what was asserted (a string literal at every call site), `ok` is its
// result. A contract body never aborts on a failed check -- it keeps going
// and lets its return value carry the total, so one bad adapter behaviour
// does not hide the rest of the report.
typedef void (*contract_report_fn)(void *ctx, const char *check, bool ok);

#ifdef __cplusplus
}
#endif

#endif  // HAC_TESTS_CONTRACT_REPORT_H
