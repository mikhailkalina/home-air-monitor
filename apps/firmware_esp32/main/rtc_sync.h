// SPDX-License-Identifier: Apache-2.0
//
// The RTC against the network's time, in short steps the event loop takes
// (phase 4f, docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md,
// "Amendment (phase 4f): time from the network"). After each SNTP answer:
//
//   1. the PCF8563's alarm armed for its next minute. The chip sets AF, and
//      its open-drain INT goes low, "when the time increments to match"
//      (pcf8563.h): at its own tick. The board stamps that edge in an
//      interrupt (platform/esp32_t5s3/board_rtc_int.h), so no task's
//      lateness moves the measurement -- only when it is looked at;
//   2. at the edge the RTC's time is a whole minute, and the system time is
//      the edge's stamp carried over to it: their difference is the RTC's
//      offset, and with the last write's mark, its rate. Logged before the
//      write. The alarm is taken back;
//   3. the write: the clock stopped, the second written and started at the
//      moment that puts its first tick on the end of that second -- by the
//      delay from a start to the first tick last measured on this chip, the
//      datasheet's until one is. The event loop is woken just before and
//      spins the rest; a moment missed is planned again for a later second,
//      RTC_SYNC_MAX_REARMS times, then the start goes out late;
//   4. the alarm armed again, and the next minute's edge says where the RTC
//      landed: the mark the next drift is measured from -- measured, never
//      assumed -- and the delay from the start to the first tick, learned
//      for the next write. On this board the datasheet's 0.508 s does not
//      hold -- its chip first ticks 986-1,000 ms after a start (2026-10-22)
//      -- so the first write after a boot lands half a second off, the next
//      ones within ±12 ms.
//
// Each step is a few I2C transfers of about a millisecond, and the write's
// spin at most RTC_SYNC_SPIN_MAX_MS; the whole takes one to three minutes of
// wall time, two minute boundaries. Every number it runs on is in
// wall_clock_limits.h.
//
// Pure over port_i2c, port_clock and the PCF8563 driver: the host runs it
// against the PCF8563 model, whose INT reports the tick's moment
// (platform/host/i2c_models/pcf8563_model.h). One task steps it.

#ifndef HAC_APPS_FIRMWARE_ESP32_RTC_SYNC_H
#define HAC_APPS_FIRMWARE_ESP32_RTC_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"
#include "port_clock.h"
#include "port_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTC_SYNC_NO_DEADLINE UINT64_MAX

// The falling edges seen on the RTC's INT: how many so far, and the last
// one's moment on port_clock's now_ms(). Whoever stamps them hands this to
// each step.
typedef struct {
    uint32_t count;
    uint64_t at_ms;
} rtc_sync_edge_t;

// The RTC against the system time at one of its ticks.
typedef struct {
    bool known;
    uint64_t wall_ms;           // the system time at the tick
    int64_t rtc_minus_wall_ms;  // positive: the RTC is ahead
    uint32_t uncertainty_ms;
} rtc_sync_tick_t;

typedef struct {
    bool known;  // both ticks, the later after, at least WALL_CLOCK_DRIFT_MIN_ELAPSED_MS apart
    uint64_t elapsed_ms;
    float ppm;              // positive: the RTC gains on the system time
    float uncertainty_ppm;  // from the two ticks' uncertainties alone
} rtc_sync_drift_t;

// The RTC's rate between `since` and `now`: how far its offset moved, over
// how long.
rtc_sync_drift_t rtc_sync_drift(const rtc_sync_tick_t *since, const rtc_sync_tick_t *now);

typedef enum {
    RTC_SYNC_IDLE = 0,
    RTC_SYNC_ARM_BEFORE,   // read the RTC, arm its alarm
    RTC_SYNC_WAIT_BEFORE,  // for the edge: the offset before the write
    RTC_SYNC_WRITE,        // for the start's moment
    RTC_SYNC_ARM_AFTER,
    RTC_SYNC_WAIT_AFTER,  // for the edge: where the write landed
} rtc_sync_phase_t;

typedef enum {
    RTC_SYNC_NOTE_COMPARED = 0,  // `tick`, `drift`: the RTC before the write
    RTC_SYNC_NOTE_NOT_COMPARED,  // `why`
    RTC_SYNC_NOTE_WRITTEN,       // the second, the start, the delay planned with
    RTC_SYNC_NOTE_WRITE_FAILED,  // `status`, `why`
    RTC_SYNC_NOTE_LANDED,        // `tick`: where it landed; the delay measured
    RTC_SYNC_NOTE_NOT_LANDED,    // `why`
} rtc_sync_note_kind_t;

// What a step did, for the log.
typedef struct {
    rtc_sync_note_kind_t kind;
    const char *why;  // NOT_*, WRITE_FAILED
    hal_status_t status;
    rtc_sync_tick_t tick;      // COMPARED, LANDED
    rtc_sync_drift_t drift;    // COMPARED
    uint64_t second_ms;        // WRITTEN: the second written, UTC
    int64_t start_late_ms;     // WRITTEN: the start against its moment
    uint32_t start_window_ms;  // WRITTEN: the start's transfer, stamped either side
    uint32_t stopped_ms;       // WRITTEN: how long the clock stood still
    uint32_t rearms;           // WRITTEN
    // WRITTEN: the delay from the start to the first tick it was planned
    // with; LANDED: the one measured, and whether it is believed -- learned.
    int64_t first_tick_ms;
    bool first_tick_believed;
} rtc_sync_note_t;

#define RTC_SYNC_NOTES_MAX 4u

typedef struct {
    port_i2c_t *bus;
    uint8_t address;
    const port_clock_t *clock;

    rtc_sync_phase_t phase;
    uint64_t deadline_ms;  // on now_ms(): the edge given up at
    uint32_t armed_count;  // the edges seen when the alarm was armed
    uint8_t alarm_minute;

    // The write.
    bool planned;
    uint64_t second_ms;    // UTC
    uint64_t start_at_ms;  // on the wall clock
    uint32_t rearms;
    uint64_t started_lo_ms;  // the start's transfer, stamped either side
    uint64_t started_hi_ms;

    // What lasts from one sync to the next.
    uint32_t first_tick_ms;  // learned, or the datasheet's
    bool first_tick_learned;
    rtc_sync_tick_t mark;  // where the RTC landed after the last write
} rtc_sync_t;

// Idle, no mark, the datasheet's delay from a start to the first tick.
void rtc_sync_init(rtc_sync_t *s, port_i2c_t *bus, uint8_t address, const port_clock_t *clock);

// An SNTP answer: from the beginning at the next step -- an alarm still
// armed taken back first.
void rtc_sync_begin(rtc_sync_t *s);

// The RTC was written by something else (time_set, rtc_stop): the alarm
// taken back if armed, the mark forgotten, idle. On the stepping task.
void rtc_sync_abort(rtc_sync_t *s);

// Does what is due at `now_ms` with `edge` as the interrupt has stamped it,
// writes what happened into `notes` -- RTC_SYNC_NOTES_MAX at most, `*count`
// of them -- and returns when to be stepped again, RTC_SYNC_NO_DEADLINE
// when idle. Earlier is harmless: a step with nothing due does nothing.
uint64_t rtc_sync_step(rtc_sync_t *s, const rtc_sync_edge_t *edge, uint64_t now_ms,
                       rtc_sync_note_t notes[RTC_SYNC_NOTES_MAX], size_t *count);

// "idle", "arming", "waiting for the RTC's minute", "waiting to write",
// "arming after the write", "waiting for where it landed"; never NULL.
const char *rtc_sync_phase_name(rtc_sync_phase_t phase);

#ifdef __cplusplus
}
#endif

#endif  // HAC_APPS_FIRMWARE_ESP32_RTC_SYNC_H
