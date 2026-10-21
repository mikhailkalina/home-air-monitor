// SPDX-License-Identifier: Apache-2.0

#include "app/link_hold.h"

#include <string.h>

link_hold_config_t link_hold_config_default(void)
{
    link_hold_config_t cfg;
    cfg.hold_ms = LINK_HOLD_DEFAULT_MS;
    return cfg;
}

void link_hold_init(link_hold_t *h, const link_hold_config_t *cfg)
{
    memset(h, 0, sizeof(*h));
    h->cfg = *cfg;
    h->shown = LINK_OFF;
}

static void show(link_hold_t *h, link_status_t state, bool *changed)
{
    if (h->shown != state) {
        h->shown = state;
        h->changes++;
        *changed = true;
    }
}

uint64_t link_hold_step(link_hold_t *h, link_status_t now, uint64_t now_ms, bool *changed)
{
    bool scratch = false;
    if (changed == NULL) {
        changed = &scratch;
    }
    *changed = false;

    // A new state starts its hold -- and so does the old one after the
    // monotonic clock restarted, whose stamp means nothing any more.
    if (!h->started || now != h->seen || now_ms < h->seen_since_ms) {
        h->seen = now;
        h->seen_since_ms = now_ms;
        h->started = true;
    }

    // No telemetry is not a link state: shown at once, never held.
    if (now == LINK_OFF) {
        show(h, LINK_OFF, changed);
        return LINK_HOLD_NO_DEADLINE;
    }
    // Telemetry, and nothing held yet: a place for the word, and no word.
    if (h->shown == LINK_OFF) {
        show(h, LINK_UNKNOWN, changed);
    }
    if (h->seen == h->shown) {
        return LINK_HOLD_NO_DEADLINE;
    }
    const uint64_t at = h->seen_since_ms + (uint64_t)h->cfg.hold_ms;
    if (now_ms < at) {
        return at;
    }
    show(h, h->seen, changed);
    return LINK_HOLD_NO_DEADLINE;
}

link_status_t link_hold_shown(const link_hold_t *h)
{
    return h->shown;
}
