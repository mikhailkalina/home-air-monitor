// SPDX-License-Identifier: Apache-2.0

#include "log_ring.h"

#include <string.h>

#define SLOT_FREE 0u
#define SLOT_WRITING 1u
#define SLOT_READY 2u

static log_ring_slot_t *slot_at(const log_ring_t *r, uint32_t index)
{
    return (log_ring_slot_t *)(void *)(r->storage +
                                       (size_t)(index % r->slot_count) * (size_t)r->slot_bytes);
}

size_t log_ring_storage_bytes(uint32_t slot_bytes, uint32_t slot_count)
{
    return (size_t)slot_bytes * (size_t)slot_count;
}

void log_ring_init(log_ring_t *r, void *storage, uint32_t slot_bytes, uint32_t slot_count)
{
    memset(r, 0, sizeof(*r));
    r->storage = (uint8_t *)storage;
    r->slot_bytes = slot_bytes;
    r->slot_count = slot_count;
    memset(storage, 0, log_ring_storage_bytes(slot_bytes, slot_count));
}

size_t log_ring_text_cap(const log_ring_t *r)
{
    return (size_t)r->slot_bytes - sizeof(log_ring_slot_t) - 1u;
}

uint32_t log_ring_used(const log_ring_t *r)
{
    return __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) -
           __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
}

log_ring_slot_t *log_ring_reserve(log_ring_t *r, uint64_t mono_ms)
{
    const uint32_t used = r->head - __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    if (used >= r->slot_count) {
        r->dropped++;
        r->dropped_total++;
        return NULL;
    }
    log_ring_slot_t *slot = slot_at(r, r->head);
    slot->level = '\0';
    slot->len = 0u;
    slot->mono_ms = mono_ms;
    __atomic_store_n(&slot->state, SLOT_WRITING, __ATOMIC_RELEASE);
    __atomic_store_n(&r->head, r->head + 1u, __ATOMIC_RELEASE);
    if (used + 1u > r->high_water) {
        r->high_water = used + 1u;
    }
    return slot;
}

void log_ring_commit(log_ring_t *r, log_ring_slot_t *slot, char level, size_t len)
{
    const size_t cap = log_ring_text_cap(r);
    if (len > cap) {
        len = cap;
        slot->text[len - 1u] = '\n';  // cut short: still one line
    }
    slot->text[len] = '\0';
    slot->len = (uint16_t)len;
    slot->level = level;
    __atomic_store_n(&slot->state, SLOT_READY, __ATOMIC_RELEASE);
}

uint32_t log_ring_take_dropped(log_ring_t *r)
{
    const uint32_t n = r->dropped;
    r->dropped = 0u;
    return n;
}

const log_ring_slot_t *log_ring_peek(log_ring_t *r, uint64_t now_ms)
{
    while (log_ring_used(r) > 0u) {
        log_ring_slot_t *slot = slot_at(r, r->tail);
        const uint8_t state = __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE);
        if (state == SLOT_READY) {
            return slot;
        }
        if (now_ms < slot->mono_ms || now_ms - slot->mono_ms < LOG_RING_STUCK_MS) {
            return NULL;  // being written: the lines behind it wait
        }
        r->lost_stuck++;
        log_ring_release(r);
    }
    return NULL;
}

void log_ring_release(log_ring_t *r)
{
    log_ring_slot_t *slot = slot_at(r, r->tail);
    __atomic_store_n(&slot->state, SLOT_FREE, __ATOMIC_RELEASE);
    __atomic_store_n(&r->tail, r->tail + 1u, __ATOMIC_RELEASE);
}
