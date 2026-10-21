// SPDX-License-Identifier: Apache-2.0

#include "rtc_sync.h"

#include <string.h>

#include "pcf8563.h"
#include "wall_clock.h"
#include "wall_clock_limits.h"

#define MS_PER_S 1000u
#define US_PER_MS 1000u
#define S_PER_MIN 60u
#define MS_PER_MIN (S_PER_MIN * MS_PER_S)
#define MINUTES_PER_HOUR 60u

// The datasheet's delay from a start to the first tick, to the millisecond
// the system time is read to: 508 ms, 0.126 ms from its middle.
#define FIRST_TICK_DATASHEET_MS ((PCF8563_FIRST_TICK_AFTER_START_US + US_PER_MS / 2u) / US_PER_MS)

// What a tick found by its edge is known to: the edge's stamp, port_clock's
// now_ms() and the system time are each read to the millisecond, truncated.
#define EDGE_UNCERTAINTY_MS 2u

// Steps one call may take through phases that need no waiting.
#define STEPS_PER_CALL 8u

static void note(rtc_sync_note_t *notes, size_t *count, const rtc_sync_note_t *n)
{
    if (*count < RTC_SYNC_NOTES_MAX) {
        notes[(*count)++] = *n;
    }
}

static void note_why(rtc_sync_note_t *notes, size_t *count, rtc_sync_note_kind_t kind,
                     const char *why, hal_status_t status)
{
    rtc_sync_note_t n;
    memset(&n, 0, sizeof(n));
    n.kind = kind;
    n.why = why;
    n.status = status;
    note(notes, count, &n);
}

rtc_sync_drift_t rtc_sync_drift(const rtc_sync_tick_t *since, const rtc_sync_tick_t *now)
{
    rtc_sync_drift_t d;
    memset(&d, 0, sizeof(d));
    if (!since->known || !now->known || now->wall_ms < since->wall_ms) {
        return d;
    }
    d.elapsed_ms = now->wall_ms - since->wall_ms;
    if (d.elapsed_ms < WALL_CLOCK_DRIFT_MIN_ELAPSED_MS) {
        return d;
    }
    const double elapsed = (double)d.elapsed_ms;
    const double moved = (double)(now->rtc_minus_wall_ms - since->rtc_minus_wall_ms);
    const double spread = (double)now->uncertainty_ms + (double)since->uncertainty_ms;
    d.known = true;
    d.ppm = (float)(moved / elapsed * 1e6);
    d.uncertainty_ppm = (float)(spread / elapsed * 1e6);
    return d;
}

void rtc_sync_init(rtc_sync_t *s, port_i2c_t *bus, uint8_t address, const port_clock_t *clock)
{
    memset(s, 0, sizeof(*s));
    s->bus = bus;
    s->address = address;
    s->clock = clock;
    s->phase = RTC_SYNC_IDLE;
    s->first_tick_ms = FIRST_TICK_DATASHEET_MS;
}

static bool armed(const rtc_sync_t *s)
{
    return s->phase == RTC_SYNC_WAIT_BEFORE || s->phase == RTC_SYNC_WAIT_AFTER;
}

void rtc_sync_begin(rtc_sync_t *s)
{
    if (armed(s)) {
        (void)pcf8563_alarm_off(s->bus, s->address);
    }
    s->phase = RTC_SYNC_ARM_BEFORE;
    s->planned = false;
    s->rearms = 0u;
}

void rtc_sync_abort(rtc_sync_t *s)
{
    if (armed(s)) {
        (void)pcf8563_alarm_off(s->bus, s->address);
    }
    s->phase = RTC_SYNC_IDLE;
    s->mark.known = false;
}

// Reads the RTC and arms its alarm for the next minute it can still catch;
// the edge is given up at `deadline_ms`. NULL, or why not.
static const char *arm(rtc_sync_t *s, const rtc_sync_edge_t *edge, uint64_t now_ms)
{
    wall_clock_rtc_t r;
    wall_clock_read_rtc(s->bus, s->address, 0u, &r);  // any valid time: compared, not believed
    if (!r.usable) {
        return wall_clock_rtc_reason(&r);
    }
    const uint32_t ahead = (r.chip.time.second > RTC_SYNC_ARM_LATEST_SECOND) ? 2u : 1u;
    s->alarm_minute = (uint8_t)((r.chip.time.minute + ahead) % MINUTES_PER_HOUR);
    s->armed_count = edge->count;  // before arming: an edge after it is the alarm's
    if (pcf8563_alarm_at_minute(s->bus, s->address, s->alarm_minute) != HAL_OK) {
        return "rtc: alarm not armed";
    }
    // The minute starts within a second after the reading says -- where in
    // its second the RTC is, is what is measured.
    const uint32_t latest_ms = (ahead * S_PER_MIN - r.chip.time.second) * MS_PER_S;
    s->deadline_ms = now_ms + latest_ms + RTC_SYNC_EDGE_GRACE_MS;
    return NULL;
}

// The tick the edge marks: the RTC was then at the alarm's whole minute,
// and the system time is the edge's stamp carried over from now_ms(). Read
// now, however long after: the RTC's time less the time since the edge is
// that minute to within its second and its drift, rounded. NULL, or why
// not.
static const char *at_edge(rtc_sync_t *s, const rtc_sync_edge_t *edge, rtc_sync_tick_t *t)
{
    memset(t, 0, sizeof(*t));
    const uint64_t wall_now = s->clock->wall_ms(s->clock);
    const uint64_t mono_now = s->clock->now_ms(s->clock);
    if (wall_now == 0u) {
        return "no wall time";
    }
    if (edge->at_ms > mono_now) {
        return "rtc: edge stamped later than now";
    }
    const uint64_t since = mono_now - edge->at_ms;
    wall_clock_rtc_t r;
    wall_clock_read_rtc(s->bus, s->address, 0u, &r);
    if (!r.usable) {
        return wall_clock_rtc_reason(&r);
    }
    if (r.unix_ms < since) {
        return "rtc: jumped";
    }
    const uint64_t minute_ms = (r.unix_ms - since + MS_PER_MIN / 2u) / MS_PER_MIN * MS_PER_MIN;
    if ((minute_ms / MS_PER_MIN) % MINUTES_PER_HOUR != s->alarm_minute) {
        return "rtc: edge not at the alarm's minute";
    }
    t->known = true;
    t->wall_ms = wall_now - since;
    t->rtc_minus_wall_ms = (int64_t)minute_ms - (int64_t)t->wall_ms;
    t->uncertainty_ms = EDGE_UNCERTAINTY_MS;
    return NULL;
}

// The second T whose end the first tick is to fall on, for a start no
// earlier than `earliest_wall`: the start is first_tick_ms before T's end.
static void plan(rtc_sync_t *s, uint64_t earliest_wall)
{
    const uint64_t end_s = (earliest_wall + s->first_tick_ms + MS_PER_S - 1u) / MS_PER_S;
    s->second_ms = (end_s - 1u) * MS_PER_S;
    s->start_at_ms = end_s * MS_PER_S - s->first_tick_ms;
    s->planned = true;
}

// Out of chances: the start goes out now, the second chosen so that the
// first tick falls nearest a second's end -- up to half a second off, which
// the tick after the write measures.
static void plan_now(rtc_sync_t *s, uint64_t wall)
{
    s->start_at_ms = wall + RTC_SYNC_WRITE_MARGIN_MS;
    const uint64_t tick_at = s->start_at_ms + s->first_tick_ms;
    const uint64_t end_s = (tick_at + MS_PER_S / 2u) / MS_PER_S;
    s->second_ms = (end_s - 1u) * MS_PER_S;
}

// The write's phase: a deadline while it is too early, else the write.
static uint64_t write_step(rtc_sync_t *s, rtc_sync_note_t *notes, size_t *count)
{
    const uint64_t wall = s->clock->wall_ms(s->clock);
    const uint64_t mono = s->clock->now_ms(s->clock);
    if (wall == 0u) {
        note_why(notes, count, RTC_SYNC_NOTE_WRITE_FAILED, "no wall time", HAL_ERR_INVALID_ARG);
        s->mark.known = false;
        s->phase = RTC_SYNC_IDLE;
        return RTC_SYNC_NO_DEADLINE;
    }
    if (!s->planned) {
        plan(s, wall + RTC_SYNC_WRITE_LEAD_MS);
    }
    if (wall + RTC_SYNC_SPIN_MAX_MS < s->start_at_ms) {
        return mono + (s->start_at_ms - RTC_SYNC_SPIN_MAX_MS - wall);  // wake nearer
    }
    bool late = false;
    if (wall + RTC_SYNC_WRITE_MARGIN_MS > s->start_at_ms) {
        if (s->rearms < RTC_SYNC_MAX_REARMS) {
            s->rearms++;
            plan(s, wall + RTC_SYNC_WRITE_LEAD_MS);
            return mono + (s->start_at_ms - RTC_SYNC_SPIN_MAX_MS - wall);
        }
        plan_now(s, wall);
        late = true;
    }

    pcf8563_time_t utc;
    pcf8563_time_t next;
    if (!wall_clock_chip_time(s->second_ms, &utc) ||
        !wall_clock_chip_time(s->second_ms + MS_PER_S, &next)) {
        note_why(notes, count, RTC_SYNC_NOTE_WRITE_FAILED, "a time the chip cannot hold",
                 HAL_ERR_INVALID_ARG);
        s->mark.known = false;
        s->phase = RTC_SYNC_IDLE;
        return RTC_SYNC_NO_DEADLINE;
    }

    const uint64_t stopped_at = s->clock->wall_ms(s->clock);
    hal_status_t st = pcf8563_stop(s->bus, s->address);
    if (st == HAL_OK) {
        st = pcf8563_write_time(s->bus, s->address, &utc);
    }
    if (st != HAL_OK) {
        (void)pcf8563_start(s->bus, s->address);  // never left stopped
        note_why(notes, count, RTC_SYNC_NOTE_WRITE_FAILED, "rtc: not written", st);
        s->mark.known = false;
        s->phase = RTC_SYNC_IDLE;
        return RTC_SYNC_NO_DEADLINE;
    }
    while (s->clock->wall_ms(s->clock) < s->start_at_ms) {
        s->clock->delay_ms(s->clock, 1u);
    }
    s->started_lo_ms = s->clock->wall_ms(s->clock);
    st = pcf8563_start(s->bus, s->address);
    s->started_hi_ms = s->clock->wall_ms(s->clock);
    if (st != HAL_OK) {
        st = pcf8563_start(s->bus, s->address);  // once more: a stopped clock is no clock
        s->started_hi_ms = s->clock->wall_ms(s->clock);
    }
    if (st != HAL_OK) {
        note_why(notes, count, RTC_SYNC_NOTE_WRITE_FAILED, "rtc: not started, left stopped", st);
        s->mark.known = false;
        s->phase = RTC_SYNC_IDLE;
        return RTC_SYNC_NO_DEADLINE;
    }

    rtc_sync_note_t n;
    memset(&n, 0, sizeof(n));
    n.kind = RTC_SYNC_NOTE_WRITTEN;
    n.status = late ? HAL_ERR_TIMEOUT : HAL_OK;
    n.second_ms = s->second_ms;
    n.start_late_ms = (int64_t)s->started_lo_ms - (int64_t)s->start_at_ms;
    n.start_window_ms = (uint32_t)(s->started_hi_ms - s->started_lo_ms);
    n.stopped_ms = (uint32_t)(s->started_lo_ms - stopped_at);
    n.rearms = s->rearms;
    n.first_tick_ms = (int64_t)s->first_tick_ms;
    note(notes, count, &n);
    s->phase = RTC_SYNC_ARM_AFTER;
    return mono;
}

// Where the write landed, from the tick the edge marks: the mark, and the
// delay from the start to the first tick -- the RTC ticks into T + 1 at the
// system time T's end less its offset.
static void landed(rtc_sync_t *s, const rtc_sync_tick_t *t, rtc_sync_note_t *notes, size_t *count)
{
    s->mark = *t;
    const uint64_t start_mid = s->started_lo_ms + (s->started_hi_ms - s->started_lo_ms + 1u) / 2u;
    const int64_t first_tick =
        (int64_t)(s->second_ms + MS_PER_S) - t->rtc_minus_wall_ms - (int64_t)start_mid;

    rtc_sync_note_t n;
    memset(&n, 0, sizeof(n));
    n.kind = RTC_SYNC_NOTE_LANDED;
    n.tick = *t;
    n.first_tick_ms = first_tick;
    n.first_tick_believed =
        first_tick >= RTC_SYNC_FIRST_TICK_MIN_MS && first_tick <= RTC_SYNC_FIRST_TICK_MAX_MS;
    if (n.first_tick_believed) {
        s->first_tick_ms = (uint32_t)first_tick;
        s->first_tick_learned = true;
    }
    note(notes, count, &n);
}

uint64_t rtc_sync_step(rtc_sync_t *s, const rtc_sync_edge_t *edge, uint64_t now_ms,
                       rtc_sync_note_t notes[RTC_SYNC_NOTES_MAX], size_t *count)
{
    *count = 0u;
    for (unsigned i = 0u; i < STEPS_PER_CALL; ++i) {
        switch (s->phase) {
            case RTC_SYNC_IDLE:
                return RTC_SYNC_NO_DEADLINE;

            case RTC_SYNC_ARM_BEFORE: {
                const char *why = arm(s, edge, now_ms);
                if (why != NULL) {
                    note_why(notes, count, RTC_SYNC_NOTE_NOT_COMPARED, why, HAL_OK);
                    s->phase = RTC_SYNC_WRITE;
                    break;
                }
                s->phase = RTC_SYNC_WAIT_BEFORE;
                return s->deadline_ms;
            }

            case RTC_SYNC_WAIT_BEFORE:
            case RTC_SYNC_WAIT_AFTER: {
                const bool before = s->phase == RTC_SYNC_WAIT_BEFORE;
                if (edge->count == s->armed_count && now_ms < s->deadline_ms) {
                    return s->deadline_ms;
                }
                rtc_sync_tick_t t;
                memset(&t, 0, sizeof(t));
                const char *why =
                    (edge->count == s->armed_count) ? "no INT edge" : at_edge(s, edge, &t);
                // INT let go: a later arming must see its edge fall.
                (void)pcf8563_alarm_off(s->bus, s->address);
                if (before) {
                    if (why != NULL) {
                        note_why(notes, count, RTC_SYNC_NOTE_NOT_COMPARED, why, HAL_OK);
                    } else {
                        rtc_sync_note_t n;
                        memset(&n, 0, sizeof(n));
                        n.kind = RTC_SYNC_NOTE_COMPARED;
                        n.tick = t;
                        n.drift = rtc_sync_drift(&s->mark, &t);
                        note(notes, count, &n);
                    }
                    s->phase = RTC_SYNC_WRITE;
                } else {
                    if (why != NULL) {
                        note_why(notes, count, RTC_SYNC_NOTE_NOT_LANDED, why, HAL_OK);
                        s->mark.known = false;
                    } else {
                        landed(s, &t, notes, count);
                    }
                    s->phase = RTC_SYNC_IDLE;
                }
                break;
            }

            case RTC_SYNC_WRITE: {
                const uint64_t due = write_step(s, notes, count);
                if (s->phase == RTC_SYNC_WRITE) {
                    return due;
                }
                break;
            }

            case RTC_SYNC_ARM_AFTER: {
                const char *why = arm(s, edge, now_ms);
                if (why != NULL) {
                    note_why(notes, count, RTC_SYNC_NOTE_NOT_LANDED, why, HAL_OK);
                    s->mark.known = false;
                    s->phase = RTC_SYNC_IDLE;
                    break;
                }
                s->phase = RTC_SYNC_WAIT_AFTER;
                return s->deadline_ms;
            }
        }
    }
    return now_ms;
}

const char *rtc_sync_phase_name(rtc_sync_phase_t phase)
{
    switch (phase) {
        case RTC_SYNC_IDLE:
            return "idle";
        case RTC_SYNC_ARM_BEFORE:
            return "arming";
        case RTC_SYNC_WAIT_BEFORE:
            return "waiting for the RTC's minute";
        case RTC_SYNC_WRITE:
            return "waiting to write";
        case RTC_SYNC_ARM_AFTER:
            return "arming after the write";
        case RTC_SYNC_WAIT_AFTER:
            return "waiting for where it landed";
    }
    return "?";
}
