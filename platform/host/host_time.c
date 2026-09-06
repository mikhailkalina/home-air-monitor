// SPDX-License-Identifier: Apache-2.0
//
// clock_gettime()/CLOCK_MONOTONIC and nanosleep() are POSIX, not C11: with
// CMAKE_C_EXTENSIONS OFF (-std=c11, not gnu11), glibc's <time.h> hides them
// unless a feature-test macro asks for POSIX explicitly. It has to be defined
// before the first system header this translation unit includes -- so before
// "host_time.h" itself, which pulls in <stdint.h> -- or it is too late.
#define _POSIX_C_SOURCE 199309L

#include "host_time.h"

#if defined(_WIN32)

#include <windows.h>

uint64_t host_monotonic_ms(void)
{
    return (uint64_t)GetTickCount64();
}

void host_sleep_ms(uint32_t ms)
{
    Sleep((DWORD)ms);
}

#else

#include <time.h>

uint64_t host_monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

void host_sleep_ms(uint32_t ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

#endif
