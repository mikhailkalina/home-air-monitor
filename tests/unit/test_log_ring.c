// SPDX-License-Identifier: Apache-2.0
//
// The debug log's ring (apps/firmware_esp32/main/log_ring.c), compiled from
// where the firmware compiles it: lines in order, a full ring dropping the
// new line and counting it, a line too long for its slot cut to one line, a
// slot still being written holding the rest back, and one never committed
// given up.

#include "log_ring.h"

#include <string.h>

#include "hac_test.h"

#define SLOT_BYTES 48u
#define SLOTS 4u

static uint64_t s_storage[(SLOT_BYTES * SLOTS) / sizeof(uint64_t)];
static log_ring_t s_ring;

static void put(const char *text, uint64_t mono_ms)
{
    log_ring_slot_t *slot = log_ring_reserve(&s_ring, mono_ms);
    HAC_CHECK(slot != NULL);
    if (slot != NULL) {
        const size_t len = strlen(text);
        const size_t cap = log_ring_text_cap(&s_ring);
        memcpy(slot->text, text, (len < cap) ? len : cap);
        log_ring_commit(&s_ring, slot, 'I', len);
    }
}

static void lines_come_out_in_order(void)
{
    log_ring_init(&s_ring, s_storage, SLOT_BYTES, SLOTS);
    put("one\n", 1u);
    put("two\n", 2u);
    const log_ring_slot_t *s = log_ring_peek(&s_ring, 10u);
    HAC_CHECK(s != NULL && strcmp(s->text, "one\n") == 0 && s->mono_ms == 1u && s->level == 'I');
    log_ring_release(&s_ring);
    s = log_ring_peek(&s_ring, 10u);
    HAC_CHECK(s != NULL && strcmp(s->text, "two\n") == 0);
    log_ring_release(&s_ring);
    HAC_CHECK(log_ring_peek(&s_ring, 10u) == NULL);
    HAC_CHECK_EQ_U64(log_ring_used(&s_ring), 0u);
}

static void a_full_ring_drops_the_new_line(void)
{
    log_ring_init(&s_ring, s_storage, SLOT_BYTES, SLOTS);
    for (unsigned i = 0u; i < SLOTS; ++i) {
        put("old\n", i);
    }
    HAC_CHECK(log_ring_reserve(&s_ring, 9u) == NULL);
    HAC_CHECK(log_ring_reserve(&s_ring, 9u) == NULL);
    HAC_CHECK_EQ_U64(log_ring_take_dropped(&s_ring), 2u);
    HAC_CHECK_EQ_U64(log_ring_take_dropped(&s_ring), 0u);
    HAC_CHECK_EQ_U64(s_ring.dropped_total, 2u);
    HAC_CHECK_EQ_U64(s_ring.high_water, SLOTS);
    // The oldest are kept, and the ring wraps once they are out.
    const log_ring_slot_t *s = log_ring_peek(&s_ring, 10u);
    HAC_CHECK(s != NULL && s->mono_ms == 0u);
    log_ring_release(&s_ring);
    put("new\n", 10u);
    unsigned n = 0u;
    for (s = log_ring_peek(&s_ring, 11u); s != NULL; s = log_ring_peek(&s_ring, 11u)) {
        n++;
        log_ring_release(&s_ring);
    }
    HAC_CHECK_EQ_U64(n, SLOTS);
}

static void a_long_line_is_cut_to_one_line(void)
{
    log_ring_init(&s_ring, s_storage, SLOT_BYTES, SLOTS);
    const size_t cap = log_ring_text_cap(&s_ring);
    char long_line[200];
    memset(long_line, 'x', sizeof(long_line) - 2u);
    long_line[sizeof(long_line) - 2u] = '\n';
    long_line[sizeof(long_line) - 1u] = '\0';
    put(long_line, 1u);
    const log_ring_slot_t *s = log_ring_peek(&s_ring, 2u);
    HAC_CHECK(s != NULL);
    HAC_CHECK_EQ_U64(s->len, cap);
    HAC_CHECK(s->text[cap - 1u] == '\n' && s->text[cap] == '\0');
    HAC_CHECK(strchr(s->text, '\n') == &s->text[cap - 1u]);
}

static void a_slot_being_written_holds_the_rest_back_until_given_up(void)
{
    log_ring_init(&s_ring, s_storage, SLOT_BYTES, SLOTS);
    log_ring_slot_t *writing = log_ring_reserve(&s_ring, 100u);
    HAC_CHECK(writing != NULL);
    put("after\n", 101u);
    HAC_CHECK(log_ring_peek(&s_ring, 100u + LOG_RING_STUCK_MS - 1u) == NULL);

    // Committed in time: both, in order.
    memcpy(writing->text, "first\n", 6u);
    log_ring_commit(&s_ring, writing, 'W', 6u);
    const log_ring_slot_t *s = log_ring_peek(&s_ring, 200u);
    HAC_CHECK(s != NULL && strcmp(s->text, "first\n") == 0 && s->level == 'W');
    log_ring_release(&s_ring);
    log_ring_release(&s_ring);

    // Never committed: given up after LOG_RING_STUCK_MS.
    (void)log_ring_reserve(&s_ring, 300u);
    put("next\n", 301u);
    HAC_CHECK(log_ring_peek(&s_ring, 300u + LOG_RING_STUCK_MS - 1u) == NULL);
    s = log_ring_peek(&s_ring, 300u + LOG_RING_STUCK_MS);
    HAC_CHECK(s != NULL && strcmp(s->text, "next\n") == 0);
    HAC_CHECK_EQ_U64(s_ring.lost_stuck, 1u);
}

int main(void)
{
    HAC_RUN(lines_come_out_in_order);
    HAC_RUN(a_full_ring_drops_the_new_line);
    HAC_RUN(a_long_line_is_cut_to_one_line);
    HAC_RUN(a_slot_being_written_holds_the_rest_back_until_given_up);
    return hac_test_summary();
}
