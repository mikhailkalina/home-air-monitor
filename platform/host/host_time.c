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

// FILETIME counts 100 ns intervals from 1601-01-01, 11,644,473,600 s before
// the Unix epoch.
#define HOST_FILETIME_PER_MS 10000u
#define HOST_FILETIME_UNIX_EPOCH_MS 11644473600000u

uint64_t host_wall_ms(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const uint64_t ticks = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
    return ticks / HOST_FILETIME_PER_MS - HOST_FILETIME_UNIX_EPOCH_MS;
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

uint64_t host_wall_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec < 0) {
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
