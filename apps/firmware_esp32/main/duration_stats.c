// SPDX-License-Identifier: Apache-2.0

#include "duration_stats.h"

#include <string.h>

void duration_stats_init(duration_stats_t *s)
{
    memset(s, 0, sizeof(*s));
}

void duration_stats_add(duration_stats_t *s, uint64_t sample_us)
{
    if (s->count == 0u || sample_us < s->min_us) {
        s->min_us = sample_us;
    }
    if (s->count == 0u || sample_us > s->max_us) {
        s->max_us = sample_us;
    }
    s->sum_us += sample_us;
    s->count++;
}

uint64_t duration_stats_mean_us(const duration_stats_t *s)
{
    if (s->count == 0u) {
        return 0u;
    }
    return (s->sum_us + (uint64_t)s->count / 2u) / (uint64_t)s->count;
}
