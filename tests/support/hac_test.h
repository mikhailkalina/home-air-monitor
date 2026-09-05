// SPDX-License-Identifier: Apache-2.0
//
// A deliberately small assertion harness: one test file is one executable, and
// CTest reports it as one test. It exists so that phase 0 adds no third-party
// dependency; the checks it offers are the ones the phase-0 tests need.

#ifndef HAC_TESTS_HAC_TEST_H
#define HAC_TESTS_HAC_TEST_H

#include <stdbool.h>
#include <stdint.h>

// Announce the test case whose checks follow.
void hac_test_begin(const char *name);

bool hac_test_check(bool ok, const char *expr, const char *file, int line);
bool hac_test_check_u64(uint64_t actual, uint64_t expected, const char *expr, const char *file,
                        int line);
bool hac_test_check_int(long long actual, long long expected, const char *expr, const char *file,
                        int line);
bool hac_test_check_f32(float actual, float expected, float eps, const char *expr, const char *file,
                        int line);

// Print the tally and return the process exit code: 0 when everything passed.
int hac_test_summary(void);

#define HAC_RUN(fn)          \
    do {                     \
        hac_test_begin(#fn); \
        fn();                \
    } while (0)

#define HAC_CHECK(cond) hac_test_check((cond) ? true : false, #cond, __FILE__, __LINE__)

#define HAC_CHECK_EQ_U64(actual, expected) \
    hac_test_check_u64((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)

#define HAC_CHECK_EQ_INT(actual, expected) \
    hac_test_check_int((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)

#define HAC_CHECK_EQ_F32(actual, expected, eps) \
    hac_test_check_f32((actual), (expected), (eps), #actual " == " #expected, __FILE__, __LINE__)

#endif  // HAC_TESTS_HAC_TEST_H
