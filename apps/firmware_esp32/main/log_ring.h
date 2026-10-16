// SPDX-License-Identifier: Apache-2.0
//
// The debug log's buffer between the tasks that log and the task that writes
// the card (docs/adr/0024-sd-card-recordings-and-debug-log.md): a ring of
// fixed-size slots, one line each, filled by any number of producers and
// emptied, in order, by one consumer.
//
// A producer RESERVES a slot -- a few instructions, under the caller's lock,
// which on the board is a spinlock -- then formats its line straight into the
// slot outside the lock, and COMMITS it. So no line is formatted twice, no
// line buffer sits on a logging task's stack, and the lock is never held
// across vsnprintf(). The consumer takes committed slots from the tail; a
// slot reserved and not yet committed holds the lines behind it back, unless
// it stays that way past LOG_RING_STUCK_MS -- a task deleted between the two
// -- when the consumer gives it up as lost.
//
// A full ring drops the new line, never an old one, and counts it: the lines
// a boot logs before a card is there survive until one is.
//
// Pure: no FreeRTOS, no ESP-IDF, the lock the caller's; built into the
// firmware from here and tested on the host from the same file
// (tests/unit/test_log_ring.c), as duration_stats.c is.

#ifndef HAC_APPS_FIRMWARE_ESP32_LOG_RING_H
#define HAC_APPS_FIRMWARE_ESP32_LOG_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// CHOICE: a slot reserved this long and not committed is given up. Long
// enough that no task merely preempted -- by the event loop's 3.5 s full
// refresh, say -- loses its line; a task that never commits was deleted.
#define LOG_RING_STUCK_MS 10000u

typedef struct {
    uint8_t state;  // free, writing, ready: read and written atomically
    char level;     // 'E', 'W', 'I', 'D', 'V', or '\0'
    uint16_t len;   // bytes of text
    uint32_t reserved_pad;
    uint64_t mono_ms;  // when the line was logged
    char text[];       // the rest of the slot
} log_ring_slot_t;

typedef struct {
    uint8_t *storage;
    uint32_t slot_bytes;  // header included
    uint32_t slot_count;
    uint32_t head;           // producers, under the lock
    uint32_t tail;           // the consumer
    uint32_t dropped;        // since log_ring_take_dropped(), under the lock
    uint32_t dropped_total;  // since boot
    uint32_t lost_stuck;     // slots given up
    uint32_t high_water;     // most slots in use at once
} log_ring_t;

// The storage a ring of `slot_count` slots of `slot_bytes` needs.
size_t log_ring_storage_bytes(uint32_t slot_bytes, uint32_t slot_count);

// `storage` is the caller's, at least log_ring_storage_bytes(), and suitably
// aligned for a uint64_t.
void log_ring_init(log_ring_t *r, void *storage, uint32_t slot_bytes, uint32_t slot_count);

// The text a slot holds, terminator excluded.
size_t log_ring_text_cap(const log_ring_t *r);

// --- producers ---------------------------------------------------------------------

// Under the caller's lock: the next slot, or NULL when the ring is full (the
// line is counted as dropped).
log_ring_slot_t *log_ring_reserve(log_ring_t *r, uint64_t mono_ms);

// Outside the lock, once `len` bytes of text are in the slot. A line cut
// short by the slot's size is made to end in '\n'.
void log_ring_commit(log_ring_t *r, log_ring_slot_t *slot, char level, size_t len);

// Under the caller's lock: the lines dropped since the last call.
uint32_t log_ring_take_dropped(log_ring_t *r);

// --- the consumer ------------------------------------------------------------------

// The oldest line, once committed; NULL when there is none, or when the
// oldest is still being written. A slot written for longer than
// LOG_RING_STUCK_MS at `now_ms` is given up and the next one looked at.
const log_ring_slot_t *log_ring_peek(log_ring_t *r, uint64_t now_ms);

// Frees what log_ring_peek() returned.
void log_ring_release(log_ring_t *r);

// Slots in use now.
uint32_t log_ring_used(const log_ring_t *r);

#ifdef __cplusplus
}
#endif

#endif  // HAC_APPS_FIRMWARE_ESP32_LOG_RING_H
