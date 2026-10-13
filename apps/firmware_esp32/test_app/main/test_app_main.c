// SPDX-License-Identifier: Apache-2.0
//
// Runs every Unity TEST_CASE linked into this image, once, at boot. Which
// ones are linked is decided at build time by `idf.py -T <component>`
// (../idf_ext.py); nothing here names a test.
//
// The last line is meant to be grepped for in a pasted log:
//
//   HAC_TEST_APP_RESULT tests=<n> failures=<n> ignored=<n> OK|FAIL
//
// An image with no test cases at all reports FAIL, not OK: it almost always
// means the build ran without -T, and a run that tested nothing must not
// read as a pass.

#include <stdbool.h>
#include <stdio.h>

#include "unity.h"

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    const int failures = UNITY_END();

    const unsigned long tests = (unsigned long)Unity.NumberOfTests;
    const bool ok = (failures == 0) && (tests > 0u);

    if (tests == 0u) {
        printf("No test cases were linked into this image. Rebuild with "
               "`idf.py -T <component> build`.\n");
    }
    printf("HAC_TEST_APP_RESULT tests=%lu failures=%d ignored=%lu %s\n", tests, failures,
           (unsigned long)Unity.TestIgnores, ok ? "OK" : "FAIL");
}
