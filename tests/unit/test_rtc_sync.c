// SPDX-License-Identifier: Apache-2.0
//
// apps/firmware_esp32/main/rtc_sync.c, compiled from where the firmware
// compiles it, against the PCF8563 model on the fake bus
// (platform/host/i2c_models/pcf8563_model.h), whose INT reports the tick's
// moment as the board's interrupt stamps it. A harness steps it as the event
// loop does -- at its deadlines, a scheduler tick at least, later when the
// loop is held -- and the system time is the virtual clock's wall time.
//
// What it holds the module to: the RTC's offset found by its tick to the
// uncertainty it states, whatever the offset and wherever in the second the
// sync starts, and however late the edge is looked at; the write landing on
// the second; a chip whose first tick after a start is not the datasheet's
// -- as this board's was not, 2026-10-21 -- learned at the first write and
// landed on the second at the next; the drift over an hour and a day; the
// write planned again when the loop is held at its moment, and sent late
// when it always is; and every way there is no edge, no valid RTC, no wall
// time or no bus, said and never leaving the clock stopped or INT held low.

#include "rtc_sync.h"

#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_i2c_fake.h"
#include "domain/civil_time.h"
#include "hac_test.h"
#include "i2c_models/pcf8563_model.h"
#include "pcf8563.h"
#include "wall_clock.h"
#include "wall_clock_limits.h"

#define RTC_ADDR 0x51u

// 2026-10-25T01:00:00Z.
#define SOME_TIME_MS 1792890000000ull

#define HOUR_MS 3600000u
#define SYNC_MS 200000u   // two minute boundaries and more
#define LOOP_TICK_MS 10u  // the event loop waits a scheduler tick at least
#define NOTES_KEPT 64u

// What the board saw on 2026-10-21: the first tick about 0.989 s after a start.
#define BOARD_FIRST_TICK_US 989000u

typedef struct {
    adp_clock_virtual_t clock;
    adp_i2c_fake_t bus;
    pcf8563_model_t chip;
    rtc_sync_edge_t edge;
    rtc_sync_t sync;
    rtc_sync_note_t notes[NOTES_KEPT];
    size_t noted;
    // The event loop held -- a full refresh, say -- over [held_from,
    // held_until) on now_ms(): no step runs there.
    uint64_t held_from;
    uint64_t held_until;
    uint32_t late_ms;  // every wake this much after its deadline
} rig_t;

static void on_int(void *ctx, uint64_t at_us)
{
    rig_t *r = (rig_t *)ctx;
    r->edge.count++;
    r->edge.at_ms = at_us / 1000u;  // esp_timer's microseconds, truncated as the board does
}

static uint8_t bcd(uint32_t v)
{
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

// 2026-10-25T01:00:00Z on the wall clock at virtual 0, `skew_ms` later, and
// an RTC `rtc_minus_wall_ms` ahead of it: its registers the second it is
// in, VL clear, running, its next tick where that offset puts it.
static void rig_init(rig_t *r, uint64_t skew_ms, int64_t rtc_minus_wall_ms)
{
    memset(r, 0, sizeof(*r));
    adp_clock_virtual_init(&r->clock, 1.0);
    adp_clock_virtual_set_wall(&r->clock, SOME_TIME_MS);
    adp_clock_virtual_advance(&r->clock, skew_ms);
    adp_i2c_fake_init(&r->bus, &r->clock);

    const uint64_t now = r->clock.now_ms;
    const uint64_t rtc = (uint64_t)((int64_t)(SOME_TIME_MS + now) + rtc_minus_wall_ms);
    const uint64_t shown = rtc / 1000u * 1000u;
    const uint64_t to_next_ms = shown + 1000u - rtc;
    pcf8563_model_init(&r->chip, now, (uint32_t)((to_next_ms % 1000u) * 1000u));
    const uint64_t held = (to_next_ms == 1000u) ? shown - 1000u : shown;  // a tick due now
    const civil_time_t t = civil_time_from_unix_ms(held);
    r->chip.regs[0] = 0x00u;
    r->chip.regs[2] = bcd(t.second);
    r->chip.regs[3] = bcd(t.minute);
    r->chip.regs[4] = bcd(t.hour);
    r->chip.regs[5] = bcd(t.day);
    r->chip.regs[6] = t.weekday;
    r->chip.regs[7] = bcd(t.month);
    r->chip.regs[8] = bcd((uint32_t)(t.year - 2000));
    pcf8563_model_on_int(&r->chip, on_int, r);
    (void)adp_i2c_fake_attach(&r->bus, RTC_ADDR, pcf8563_model_port(&r->chip));
    rtc_sync_init(&r->sync, adp_i2c_fake_port(&r->bus), RTC_ADDR,
                  adp_clock_virtual_port(&r->clock));
}

// One step at the virtual clock's now, as the event loop's wake: the RTC
// first brought up to now, so that its INT has fallen if a tick made it.
static uint64_t step_once(rig_t *r)
{
    pcf8563_model_advance(&r->chip, r->clock.now_ms);
    rtc_sync_note_t notes[RTC_SYNC_NOTES_MAX];
    size_t n = 0u;
    const uint64_t due = rtc_sync_step(&r->sync, &r->edge, r->clock.now_ms, notes, &n);
    for (size_t i = 0u; i < n && r->noted < NOTES_KEPT; ++i) {
        r->notes[r->noted++] = notes[i];
    }
    return due;
}

// Steps until idle or `for_ms` has passed, sleeping to each deadline.
static void run(rig_t *r, uint64_t for_ms)
{
    const uint64_t until = r->clock.now_ms + for_ms;
    while (r->clock.now_ms < until) {
        const uint64_t due = step_once(r);
        if (due == RTC_SYNC_NO_DEADLINE) {
            return;
        }
        uint64_t next = (due > r->clock.now_ms) ? due : r->clock.now_ms + LOOP_TICK_MS;
        next += r->late_ms;
        if (next >= r->held_from && next < r->held_until) {
            next = r->held_until;
        }
        if (next > until) {
            next = until;
        }
        adp_clock_virtual_advance(&r->clock, next - r->clock.now_ms);
    }
}

// Steps until the sync reaches `phase`, at most `for_ms`.
static void run_until_phase(rig_t *r, rtc_sync_phase_t phase, uint64_t for_ms)
{
    const uint64_t until = r->clock.now_ms + for_ms;
    while (r->sync.phase != phase && r->clock.now_ms < until) {
        const uint64_t due = step_once(r);
        if (r->sync.phase == phase || due == RTC_SYNC_NO_DEADLINE) {
            return;
        }
        const uint64_t next = (due > r->clock.now_ms) ? due : r->clock.now_ms + LOOP_TICK_MS;
        adp_clock_virtual_advance(&r->clock, next - r->clock.now_ms);
    }
}

static void sync_now(rig_t *r)
{
    r->noted = 0u;
    rtc_sync_begin(&r->sync);
    run(r, SYNC_MS);
}

// The `nth` note of `kind` since the last sync_now(), or NULL.
static const rtc_sync_note_t *find(const rig_t *r, rtc_sync_note_kind_t kind, unsigned nth)
{
    for (size_t i = 0u; i < r->noted; ++i) {
        if (r->notes[i].kind == kind && nth-- == 0u) {
            return &r->notes[i];
        }
    }
    return NULL;
}

static int64_t abs64(int64_t v)
{
    return (v < 0) ? -v : v;
}

// After any sync: the RTC running, its alarm off, INT let go.
static void check_left_clean(const rig_t *r)
{
    HAC_CHECK(pcf8563_model_running(&r->chip));
    HAC_CHECK((r->chip.regs[1] & 0x0Au) == 0u);  // AF and AIE clear
    HAC_CHECK((r->chip.regs[9] & 0x80u) != 0u);  // the minute alarm off
    HAC_CHECK(!r->chip.int_low);
    HAC_CHECK_EQ_INT(r->sync.phase, RTC_SYNC_IDLE);
}

// Whatever the RTC's offset and wherever in the second the sync starts: the
// offset before the write to its uncertainty, the write on time, and the
// RTC landed on the second by the datasheet's delay, which the model keeps.
static void the_offset_before_and_the_landing_after(void)
{
    const int64_t offsets[] = {0, 1, -1, 300, -481, 999, -999, -12918, 86400000, -3600123};
    const uint64_t skews[] = {0u, 497u, 998u};
    for (size_t i = 0u; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        for (size_t j = 0u; j < sizeof(skews) / sizeof(skews[0]); ++j) {
            rig_t r;
            rig_init(&r, skews[j], offsets[i]);
            sync_now(&r);
            const rtc_sync_note_t *c = find(&r, RTC_SYNC_NOTE_COMPARED, 0u);
            const rtc_sync_note_t *w = find(&r, RTC_SYNC_NOTE_WRITTEN, 0u);
            const rtc_sync_note_t *l = find(&r, RTC_SYNC_NOTE_LANDED, 0u);
            HAC_CHECK(c != NULL && w != NULL && l != NULL);
            if (c == NULL || w == NULL || l == NULL) {
                continue;
            }
            HAC_CHECK(c->tick.known);
            HAC_CHECK(abs64(c->tick.rtc_minus_wall_ms - offsets[i]) <=
                      (int64_t)c->tick.uncertainty_ms);
            HAC_CHECK(!c->drift.known);  // no mark yet
            HAC_CHECK_EQ_INT(w->status, HAL_OK);
            HAC_CHECK_EQ_U64(w->rearms, 0u);
            HAC_CHECK(w->start_late_ms >= 0 && w->start_late_ms <= 1);
            HAC_CHECK(w->stopped_ms <= RTC_SYNC_SPIN_MAX_MS + RTC_SYNC_WRITE_MARGIN_MS);
            HAC_CHECK(w->second_ms % 1000u == 0u);
            HAC_CHECK(abs64(l->tick.rtc_minus_wall_ms) <= (int64_t)l->tick.uncertainty_ms);
            HAC_CHECK(l->first_tick_believed);
            HAC_CHECK(abs64(l->first_tick_ms - 508) <= 2);
            HAC_CHECK(r.sync.mark.known);
            HAC_CHECK_EQ_U64(r.noted, 3u);
            check_left_clean(&r);
        }
    }
}

// The board's chip, 2026-10-21: its first tick about 0.989 s after a start.
// The first write lands 0.48 s behind and says so; the delay is learned, and
// the next write lands on the second.
static void a_chip_that_ticks_late_after_a_start_is_learned(void)
{
    rig_t r;
    rig_init(&r, 123u, -12918);
    r.chip.first_tick_after_start_us = BOARD_FIRST_TICK_US;
    sync_now(&r);
    const rtc_sync_note_t *l = find(&r, RTC_SYNC_NOTE_LANDED, 0u);
    HAC_CHECK(l != NULL);
    if (l == NULL) {
        return;
    }
    HAC_CHECK(abs64(l->tick.rtc_minus_wall_ms + 481) <= 2);
    HAC_CHECK(l->first_tick_believed);
    HAC_CHECK(abs64(l->first_tick_ms - 989) <= 2);

    adp_clock_virtual_advance(&r.clock, HOUR_MS);
    sync_now(&r);
    const rtc_sync_note_t *c = find(&r, RTC_SYNC_NOTE_COMPARED, 0u);
    const rtc_sync_note_t *w = find(&r, RTC_SYNC_NOTE_WRITTEN, 0u);
    l = find(&r, RTC_SYNC_NOTE_LANDED, 0u);
    HAC_CHECK(c != NULL && w != NULL && l != NULL);
    if (c == NULL || w == NULL || l == NULL) {
        return;
    }
    HAC_CHECK(abs64(c->tick.rtc_minus_wall_ms + 481) <= 3);  // where it was left
    HAC_CHECK(c->drift.known);
    HAC_CHECK_EQ_F32(c->drift.ppm, 0.0f, 1.5f);
    HAC_CHECK(abs64(w->first_tick_ms - 989) <= 2);
    HAC_CHECK(abs64(l->tick.rtc_minus_wall_ms) <= 2);
    check_left_clean(&r);
}

// The crystal's error, an hour after a write: -29 ppm, as the board's first
// answer suggested, loses some 104 ms.
static void the_drift_over_an_hour_is_the_crystals(void)
{
    const int32_t ppms[] = {-29, 20, 0, 3};
    for (size_t i = 0u; i < sizeof(ppms) / sizeof(ppms[0]); ++i) {
        rig_t r;
        rig_init(&r, 0u, 640);
        r.chip.ppm = ppms[i];
        sync_now(&r);
        HAC_CHECK(r.sync.mark.known);
        adp_clock_virtual_advance(&r.clock, HOUR_MS);
        sync_now(&r);
        const rtc_sync_note_t *c = find(&r, RTC_SYNC_NOTE_COMPARED, 0u);
        HAC_CHECK(c != NULL);
        if (c == NULL) {
            continue;
        }
        HAC_CHECK(c->drift.known);
        HAC_CHECK(c->drift.elapsed_ms >= HOUR_MS);
        HAC_CHECK_EQ_F32(c->drift.ppm, (float)ppms[i], 1.5f);
        HAC_CHECK(c->drift.uncertainty_ppm > 0.0f && c->drift.uncertainty_ppm < 1.5f);
        // ppm x elapsed, in ms: an hour and the minute or two the sync took.
        const int64_t expected = (int64_t)ppms[i] * (int64_t)c->drift.elapsed_ms / 1000000;
        HAC_CHECK(abs64(c->tick.rtc_minus_wall_ms - expected) <= 4);
    }
}

// What the board does with nobody watching: a sync an hour for a day, every
// hour's rate the crystal's.
static void a_day_of_hourly_syncs_measures_the_drift_every_hour(void)
{
    rig_t r;
    rig_init(&r, 0u, -12918);
    r.chip.ppm = 17;
    r.chip.first_tick_after_start_us = BOARD_FIRST_TICK_US;
    unsigned measured = 0u;
    for (unsigned hour = 0u; hour < 24u; ++hour) {
        sync_now(&r);
        const rtc_sync_note_t *c = find(&r, RTC_SYNC_NOTE_COMPARED, 0u);
        HAC_CHECK(c != NULL);
        if (c != NULL && c->drift.known) {
            HAC_CHECK_EQ_F32(c->drift.ppm, 17.0f, 1.5f);
            measured++;
        }
        HAC_CHECK(find(&r, RTC_SYNC_NOTE_LANDED, 0u) != NULL);
        adp_clock_virtual_advance(&r.clock, HOUR_MS);
    }
    HAC_CHECK_EQ_U64(measured, 23u);
    check_left_clean(&r);
}

// The loop held at the start's moment -- a full refresh, 3.7 s: the write
// planned again for a later second and landed on it.
static void a_held_loop_plans_the_write_again(void)
{
    rig_t r;
    rig_init(&r, 0u, 250);
    r.noted = 0u;
    rtc_sync_begin(&r.sync);
    run_until_phase(&r, RTC_SYNC_WRITE, SYNC_MS);
    HAC_CHECK_EQ_INT(r.sync.phase, RTC_SYNC_WRITE);
    r.held_from = r.clock.now_ms;
    r.held_until = r.clock.now_ms + 3700u;
    run(&r, SYNC_MS);
    const rtc_sync_note_t *w = find(&r, RTC_SYNC_NOTE_WRITTEN, 0u);
    const rtc_sync_note_t *l = find(&r, RTC_SYNC_NOTE_LANDED, 0u);
    HAC_CHECK(w != NULL && l != NULL);
    if (w == NULL || l == NULL) {
        return;
    }
    HAC_CHECK(w->rearms >= 1u);
    HAC_CHECK_EQ_INT(w->status, HAL_OK);
    HAC_CHECK(abs64(l->tick.rtc_minus_wall_ms) <= 2);
    check_left_clean(&r);
}

// Every wake half a second late: the set-ups run out, the start goes out
// late, and the tick after says where it landed -- measured, so the next
// drift is right all the same.
static void a_loop_always_late_starts_late_and_measures_it(void)
{
    rig_t r;
    rig_init(&r, 0u, 250);
    r.late_ms = 500u;
    sync_now(&r);
    const rtc_sync_note_t *w = find(&r, RTC_SYNC_NOTE_WRITTEN, 0u);
    const rtc_sync_note_t *l = find(&r, RTC_SYNC_NOTE_LANDED, 0u);
    HAC_CHECK(w != NULL && l != NULL);
    if (w == NULL || l == NULL) {
        return;
    }
    HAC_CHECK_EQ_U64(w->rearms, RTC_SYNC_MAX_REARMS);
    HAC_CHECK_EQ_INT(w->status, HAL_ERR_TIMEOUT);
    HAC_CHECK(abs64(l->tick.rtc_minus_wall_ms) <= 500);
    HAC_CHECK(l->first_tick_believed);
    HAC_CHECK(abs64(l->first_tick_ms - 508) <= 2);  // the start's stamps, not its plan
    HAC_CHECK(r.sync.mark.known);
    check_left_clean(&r);
}

// The edge looked at twenty seconds late: the interrupt's stamp, not the
// look, is the measurement.
static void an_edge_looked_at_late_is_still_exact(void)
{
    rig_t r;
    rig_init(&r, 0u, -777);
    r.noted = 0u;
    rtc_sync_begin(&r.sync);
    run_until_phase(&r, RTC_SYNC_WAIT_BEFORE, SYNC_MS);
    r.held_from = r.clock.now_ms;
    r.held_until = r.sync.deadline_ms - RTC_SYNC_EDGE_GRACE_MS + 20000u;
    run(&r, SYNC_MS);
    const rtc_sync_note_t *c = find(&r, RTC_SYNC_NOTE_COMPARED, 0u);
    HAC_CHECK(c != NULL);
    if (c != NULL) {
        HAC_CHECK(abs64(c->tick.rtc_minus_wall_ms + 777) <= (int64_t)c->tick.uncertainty_ms);
    }
}

// INT wired to nothing: no edge, said twice; the RTC written all the same,
// to the second; no mark to measure a drift from.
static void no_int_edge_is_said_and_the_rtc_written(void)
{
    rig_t r;
    rig_init(&r, 0u, 5000);
    pcf8563_model_on_int(&r.chip, NULL, NULL);
    sync_now(&r);
    const rtc_sync_note_t *nc = find(&r, RTC_SYNC_NOTE_NOT_COMPARED, 0u);
    const rtc_sync_note_t *nl = find(&r, RTC_SYNC_NOTE_NOT_LANDED, 0u);
    HAC_CHECK(nc != NULL && nc->why != NULL && strcmp(nc->why, "no INT edge") == 0);
    HAC_CHECK(find(&r, RTC_SYNC_NOTE_WRITTEN, 0u) != NULL);
    HAC_CHECK(nl != NULL && nl->why != NULL && strcmp(nl->why, "no INT edge") == 0);
    HAC_CHECK(!r.sync.mark.known);
    check_left_clean(&r);
    // The 5 s the RTC was ahead are gone.
    wall_clock_rtc_t rtc;
    wall_clock_read_rtc(adp_i2c_fake_port(&r.bus), RTC_ADDR, 0u, &rtc);
    const uint64_t wall = r.clock.port.wall_ms(&r.clock.port);
    HAC_CHECK(rtc.usable && abs64((int64_t)rtc.unix_ms - (int64_t)wall) <= 1000);
}

// An RTC that lost its time, or was stopped: nothing to compare, written,
// landed on the second.
static void an_rtc_without_a_time_is_written_not_compared(void)
{
    const struct {
        uint8_t reg;
        uint8_t set;
        const char *why;
    } cases[] = {{2u, 0x80u, "rtc: VL set"}, {0u, 0x20u, "rtc: stopped"}};
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        rig_t r;
        rig_init(&r, 0u, 0);
        r.chip.regs[cases[i].reg] |= cases[i].set;
        sync_now(&r);
        const rtc_sync_note_t *nc = find(&r, RTC_SYNC_NOTE_NOT_COMPARED, 0u);
        HAC_CHECK(nc != NULL && nc->why != NULL && strcmp(nc->why, cases[i].why) == 0);
        const rtc_sync_note_t *l = find(&r, RTC_SYNC_NOTE_LANDED, 0u);
        HAC_CHECK(l != NULL && abs64(l->tick.rtc_minus_wall_ms) <= 2);
        HAC_CHECK((r.chip.regs[2] & 0x80u) == 0u);  // VL cleared
        check_left_clean(&r);
    }
}

// time_set or rtc_stop meanwhile: the alarm taken back, the mark forgotten.
static void an_abort_takes_the_alarm_back(void)
{
    rig_t r;
    rig_init(&r, 0u, 0);
    sync_now(&r);
    HAC_CHECK(r.sync.mark.known);
    r.noted = 0u;
    rtc_sync_begin(&r.sync);
    run_until_phase(&r, RTC_SYNC_WAIT_BEFORE, SYNC_MS);
    HAC_CHECK((r.chip.regs[1] & 0x02u) != 0u);  // armed
    rtc_sync_abort(&r.sync);
    HAC_CHECK(!r.sync.mark.known);
    run(&r, SYNC_MS);
    HAC_CHECK_EQ_U64(r.noted, 0u);
    check_left_clean(&r);
}

// A second answer while a sync runs: from the beginning, one comparison.
static void an_answer_during_a_sync_starts_it_again(void)
{
    rig_t r;
    rig_init(&r, 0u, 333);
    r.noted = 0u;
    rtc_sync_begin(&r.sync);
    run_until_phase(&r, RTC_SYNC_WAIT_BEFORE, SYNC_MS);
    adp_clock_virtual_advance(&r.clock, 5000u);
    rtc_sync_begin(&r.sync);
    run(&r, SYNC_MS);
    HAC_CHECK(find(&r, RTC_SYNC_NOTE_COMPARED, 0u) != NULL);
    HAC_CHECK(find(&r, RTC_SYNC_NOTE_COMPARED, 1u) == NULL);
    HAC_CHECK(find(&r, RTC_SYNC_NOTE_LANDED, 0u) != NULL);
    check_left_clean(&r);
}

// An RTC at second 58: armed for the minute after next, not an hour late.
static void second_58_waits_for_the_minute_after(void)
{
    rig_t r;
    rig_init(&r, 0u, 58500);
    r.noted = 0u;
    rtc_sync_begin(&r.sync);
    run_until_phase(&r, RTC_SYNC_WAIT_BEFORE, SYNC_MS);
    HAC_CHECK_EQ_INT(r.sync.alarm_minute, 2);
    const uint64_t armed_at = r.clock.now_ms;
    run(&r, SYNC_MS);
    const rtc_sync_note_t *c = find(&r, RTC_SYNC_NOTE_COMPARED, 0u);
    HAC_CHECK(c != NULL && abs64(c->tick.rtc_minus_wall_ms - 58500) <= 2);
    HAC_CHECK(r.clock.now_ms - armed_at < 2u * SYNC_MS);
}

static void no_wall_time_is_said(void)
{
    rig_t r;
    rig_init(&r, 0u, 0);
    r.clock.wall_synced = false;
    sync_now(&r);
    const rtc_sync_note_t *nc = find(&r, RTC_SYNC_NOTE_NOT_COMPARED, 0u);
    const rtc_sync_note_t *wf = find(&r, RTC_SYNC_NOTE_WRITE_FAILED, 0u);
    HAC_CHECK(nc != NULL && nc->why != NULL && strcmp(nc->why, "no wall time") == 0);
    HAC_CHECK(wf != NULL && wf->status == HAL_ERR_INVALID_ARG);
    check_left_clean(&r);
}

// The chip's last second: its tick would go into 2100.
static void a_time_past_the_chips_is_not_written(void)
{
    rig_t r;
    rig_init(&r, 0u, 0);
    adp_clock_virtual_set_wall(&r.clock, 4102444798000ull);  // 2099-12-31T23:59:58Z
    sync_now(&r);
    const rtc_sync_note_t *wf = find(&r, RTC_SYNC_NOTE_WRITE_FAILED, 0u);
    HAC_CHECK(wf != NULL && wf->status == HAL_ERR_INVALID_ARG);
    check_left_clean(&r);
}

// The bus hung at the write: said, and the clock not left stopped.
static void a_bus_that_fails_at_the_write_leaves_the_clock_running(void)
{
    rig_t r;
    rig_init(&r, 0u, 0);
    r.noted = 0u;
    rtc_sync_begin(&r.sync);
    run_until_phase(&r, RTC_SYNC_WRITE, SYNC_MS);
    adp_i2c_fake_hang(&r.bus, 1u);
    run(&r, SYNC_MS);
    const rtc_sync_note_t *wf = find(&r, RTC_SYNC_NOTE_WRITE_FAILED, 0u);
    HAC_CHECK(wf != NULL && wf->status == HAL_ERR_TIMEOUT);
    HAC_CHECK(!r.sync.mark.known);
    HAC_CHECK(pcf8563_model_running(&r.chip));
}

static void no_drift_without_a_mark_or_time_between(void)
{
    rtc_sync_tick_t t = {
        .known = true, .wall_ms = SOME_TIME_MS, .rtc_minus_wall_ms = 50, .uncertainty_ms = 1u};
    rtc_sync_tick_t none = {.known = false};
    HAC_CHECK(!rtc_sync_drift(&none, &t).known);

    rtc_sync_tick_t recent = {.known = true,
                              .wall_ms = SOME_TIME_MS - WALL_CLOCK_DRIFT_MIN_ELAPSED_MS + 1u,
                              .rtc_minus_wall_ms = 0,
                              .uncertainty_ms = 1u};
    HAC_CHECK(!rtc_sync_drift(&recent, &t).known);
    recent.wall_ms = SOME_TIME_MS - WALL_CLOCK_DRIFT_MIN_ELAPSED_MS;
    const rtc_sync_drift_t d = rtc_sync_drift(&recent, &t);
    HAC_CHECK(d.known);
    HAC_CHECK_EQ_F32(d.ppm, 50.0f / 600000.0f * 1e6f, 0.01f);
    HAC_CHECK_EQ_F32(d.uncertainty_ppm, 2.0f / 600000.0f * 1e6f, 0.01f);

    rtc_sync_tick_t later = recent;
    later.wall_ms = SOME_TIME_MS + 1u;
    HAC_CHECK(!rtc_sync_drift(&later, &t).known);
    t.known = false;
    HAC_CHECK(!rtc_sync_drift(&recent, &t).known);
}

static void phases_have_names(void)
{
    HAC_CHECK(strcmp(rtc_sync_phase_name(RTC_SYNC_IDLE), "idle") == 0);
    HAC_CHECK(strcmp(rtc_sync_phase_name(RTC_SYNC_WAIT_BEFORE), "waiting for the RTC's minute") ==
              0);
    HAC_CHECK(strcmp(rtc_sync_phase_name(RTC_SYNC_WAIT_AFTER), "waiting for where it landed") == 0);
}

int main(void)
{
    HAC_RUN(the_offset_before_and_the_landing_after);
    HAC_RUN(a_chip_that_ticks_late_after_a_start_is_learned);
    HAC_RUN(the_drift_over_an_hour_is_the_crystals);
    HAC_RUN(a_day_of_hourly_syncs_measures_the_drift_every_hour);
    HAC_RUN(a_held_loop_plans_the_write_again);
    HAC_RUN(a_loop_always_late_starts_late_and_measures_it);
    HAC_RUN(an_edge_looked_at_late_is_still_exact);
    HAC_RUN(no_int_edge_is_said_and_the_rtc_written);
    HAC_RUN(an_rtc_without_a_time_is_written_not_compared);
    HAC_RUN(an_abort_takes_the_alarm_back);
    HAC_RUN(an_answer_during_a_sync_starts_it_again);
    HAC_RUN(second_58_waits_for_the_minute_after);
    HAC_RUN(no_wall_time_is_said);
    HAC_RUN(a_time_past_the_chips_is_not_written);
    HAC_RUN(a_bus_that_fails_at_the_write_leaves_the_clock_running);
    HAC_RUN(no_drift_without_a_mark_or_time_between);
    HAC_RUN(phases_have_names);
    return hac_test_summary();
}
