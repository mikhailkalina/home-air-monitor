// SPDX-License-Identifier: Apache-2.0
//
// Every number the wall clock's time from the network runs on (phase 4f,
// docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md, "Amendment
// (phase 4f): time from the network"): how rtc_sync.c measures the RTC by
// its alarm's tick and sets it to a second's start, and how net_time.c asks
// the network. Each says what it is; docs/hardware/board_notes.md, open item
// "Time from the network on the board", says which log line gives each its
// value.

#ifndef HAC_APPS_FIRMWARE_ESP32_WALL_CLOCK_LIMITS_H
#define HAC_APPS_FIRMWARE_ESP32_WALL_CLOCK_LIMITS_H

// --- The RTC's tick, by its alarm (rtc_sync.c) -------------------------------

// CHOICE: the alarm is armed for the next minute only while that minute is
// at least two seconds away by the RTC's own reading; from second 58 on, for
// the one after -- an increment between the reading and the arming would
// otherwise be missed, and the alarm come an hour late.
#define RTC_SYNC_ARM_LATEST_SECOND 57u

// CHOICE: the RTC's minute starts within a second of when its reading says
// -- its phase is what is being measured -- and its tick is waited for this
// long past the latest moment it can come before the pin is given up on:
// "no INT edge". Once a second of slack for a reading late on the bus, half
// a second for the event loop's own wake.
#define RTC_SYNC_EDGE_GRACE_MS 1500u

// --- The write (rtc_sync.c) ---------------------------------------------------

// CHOICE: a write is planned for a start at least this long after it is
// decided, a pass of the event loop and more: the start's moment is a
// deadline the loop is woken for.
#define RTC_SYNC_WRITE_LEAD_MS 200u

// CHOICE: the event loop is woken this long before the start's moment and
// spins the rest -- the clock stopped and its time written first, then
// single milliseconds. On the board a wake comes within a scheduler tick,
// 10 ms, of its deadline, either side (event_loop.c's wait in ticks); 30 ms
// of spinning an hour is far inside BSEC's 187 ms tolerance, and the clock
// stands still for that long at most.
#define RTC_SYNC_SPIN_MAX_MS 30u

// CHOICE: the stop and the time's write, two transfers of about a
// millisecond each at 100 kHz, need this much before the start's moment; a
// wake later than that has missed it.
#define RTC_SYNC_WRITE_MARGIN_MS 3u

// CHOICE: a missed start is planned again for a later second this many
// times; then the clock is started where the event loop is, late, and the
// tick after says where it landed. A full refresh holds the event loop up
// to 3.7 s today (ADR 0008); the display task of ADR 0022 would not.
#define RTC_SYNC_MAX_REARMS 3u

// What a learned delay from the start to the first tick must lie within to
// be believed. The datasheet's is 507.874 ms (pcf8563.h); this board's chip
// takes about a second, 986-1,000 ms over the starts of 2026-10-22
// (board_notes.md) -- so a learned delay lands the next write within ±12 ms.
// Anything outside is a measurement gone wrong, and the last believed value
// stays.
#define RTC_SYNC_FIRST_TICK_MIN_MS 100u
#define RTC_SYNC_FIRST_TICK_MAX_MS 1900u

// --- The drift (rtc_sync_drift()) ---------------------------------------------

// CHOICE: a rate is worked out only over at least this long. Two ticks each
// found to about a millisecond, ten minutes apart, give the rate to some
// 3 ppm; the hourly sync's, to under 1 ppm. That is the RTC against the
// system time; the system time's own error against UTC comes on top. SNTP
// as ESP-IDF builds lwIP takes the server's time as the packet arrives, with
// no round-trip correction (SNTP_COMP_ROUNDTRIP 0), so each answer lags UTC
// by the packet's one-way trip -- milliseconds to tens of them -- and the
// ESP32's own crystal drifts the system time over the minute or two between
// the answer and the RTC's tick. What varies from hour to hour is noise on
// each sample, which a day's samples average.
#define WALL_CLOCK_DRIFT_MIN_ELAPSED_MS 600000u

// --- Asking the network (net_time.c) -----------------------------------------

// CHOICE: how often SNTP asks once it has an answer: ESP-IDF's own default
// (CONFIG_LWIP_SNTP_UPDATE_DELAY), an hour, set here so that it is this
// project's choice and not a default's. Each answer sets the system time,
// and measures and writes the RTC: at the -29 ppm first seen on this board,
// 105 ms an hour against the millisecond a tick is found to. Without an
// answer lwIP asks again 15 s later, doubling to 150 s (lwIP's
// SNTP_RETRY_TIMEOUT and _MAX).
#define NET_TIME_SYNC_INTERVAL_MS 3600000u

#endif  // HAC_APPS_FIRMWARE_ESP32_WALL_CLOCK_LIMITS_H
