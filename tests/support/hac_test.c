// SPDX-License-Identifier: Apache-2.0

#include "hac_test.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>

static const char *s_current_case = "<none>";
static unsigned s_checks;
static unsigned s_failures;

void hac_test_begin(const char *name)
{
    s_current_case = name;
}

static void report_failure(const char *expr, const char *file, int line)
{
    s_failures++;
    fprintf(stderr, "FAIL %s\n  %s:%d\n  %s\n", s_current_case, file, line, expr);
}

bool hac_test_check(bool ok, const char *expr, const char *file, int line)
{
    s_checks++;
    if (!ok) {
        report_failure(expr, file, line);
    }
    return ok;
}

bool hac_test_check_u64(uint64_t actual, uint64_t expected, const char *expr, const char *file,
                        int line)
{
    s_checks++;
    if (actual == expected) {
        return true;
    }
    s_failures++;
    fprintf(stderr, "FAIL %s\n  %s:%d\n  %s\n  actual   %" PRIu64 "\n  expected %" PRIu64 "\n",
            s_current_case, file, line, expr, actual, expected);
    return false;
}

bool hac_test_check_int(long long actual, long long expected, const char *expr, const char *file,
                        int line)
{
    s_checks++;
    if (actual == expected) {
        return true;
    }
    s_failures++;
    fprintf(stderr, "FAIL %s\n  %s:%d\n  %s\n  actual   %lld\n  expected %lld\n", s_current_case,
            file, line, expr, actual, expected);
    return false;
}

bool hac_test_check_f32(float actual, float expected, float eps, const char *expr, const char *file,
                        int line)
{
    s_checks++;
    if (fabsf(actual - expected) <= eps) {
        return true;
    }
    s_failures++;
    fprintf(stderr, "FAIL %s\n  %s:%d\n  %s\n  actual   %f\n  expected %f (+/- %f)\n",
            s_current_case, file, line, expr, (double)actual, (double)expected, (double)eps);
    return false;
}

int hac_test_summary(void)
{
    if (s_failures == 0u) {
        printf("PASS: %u checks\n", s_checks);
        return 0;
    }
    printf("FAILED: %u of %u checks\n", s_failures, s_checks);
    return 1;
}
