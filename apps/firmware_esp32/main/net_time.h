// SPDX-License-Identifier: Apache-2.0
//
// Time from the network (phase 4f,
// docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md, "Amendment
// (phase 4f): time from the network"): SNTP through ESP-IDF's
// esp_netif_sntp, set going once Wi-Fi first has an address, and after each
// answer the RTC measured against it and written -- rtc_sync.h's steps, on
// the event loop, which net_time_step() is the hook of. No task of its own.
//
// Who does what:
//
//   * lwIP's task: sntp_sync_time(), the function ESP-IDF declares weak for
//     the moment an answer arrives and documents as the app's to replace
//     (components/lwip/include/apps/esp_sntp.h). This file replaces it: the
//     server's time held to the years this firmware believes
//     (wall_clock_believable_ms()), the system time set from it at once --
//     settimeofday(), as ESP-IDF's own does -- or refused; the answer left
//     for the event loop, which is woken. Nothing else runs there;
//   * ESP-IDF's event task: Wi-Fi's address, left and woken for likewise;
//   * the RTC's INT, IO9: its edges stamped in an interrupt
//     (platform/esp32_t5s3/board_rtc_int.h);
//   * the event loop: net_time_step() starts SNTP at the first address,
//     logs each answer, sets the wall clock's source to SNTP and steps
//     rtc_sync -- a few I2C transfers a step, a spin of 30 ms at most an
//     hour -- logging what each step did.
//
// So the card's log/ carries one drift sample an hour with nobody watching.
// The lines, tag net_time:
//
//   waiting for the network's first address server=... interval_s=...
//       rtc_alarm_off=...
//   sntp started server=... interval_s=... esp_err=...
//   synced utc=...Z system_minus_sntp_ms=... source_was=... syncs=...
//   rtc against sntp rtc_minus_sntp_ms=... uncertainty_ms=... since_write_s=...
//       drift_ppm=... drift_uncertainty_ppm=...
//   rtc written utc=...Z first_tick_ms=... start_late_ms=... stopped_ms=...
//       rearms=... start_window_ms=...
//   rtc landed residual_ms=... uncertainty_ms=... first_tick_ms=... learned=...
//   (WARN) rtc against sntp: nothing to compare why=...
//   (WARN) rtc landed: not seen why=...
//   (ERROR) rtc write failed why=... status=...
//   (WARN) time refused: before 2026 or from 2100 on, nothing set ...
//
// Without a network in the image the composition never calls into it but
// for net_time_status() and net_time_forget_rtc_mark(), and the linker drops
// SNTP with the rest of the stack.

#ifndef HAC_APPS_FIRMWARE_ESP32_NET_TIME_H
#define HAC_APPS_FIRMWARE_ESP32_NET_TIME_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_status.h"
#include "port_clock.h"
#include "port_i2c.h"
#include "port_log.h"
#include "rtc_sync.h"
#include "wall_clock.h"

#ifdef __cplusplus
extern "C" {
#endif

// Wakes the event loop; from any task, never waiting.
typedef void (*net_time_wake_fn)(void);

typedef struct {
    port_i2c_t *bus;  // the RTC's
    uint8_t rtc_address;
    const port_clock_t *clock;  // its wall_ms() is the system time SNTP sets
    const port_log_t *log;
    // Set to WALL_CLOCK_SOURCE_SNTP at each answer, on the event loop. The
    // console and the event loop read it, a word.
    wall_clock_source_t *wall_source;
    const char *server;  // a name or an address; borrowed for the firmware's life
    net_time_wake_fn wake;
} net_time_config_t;

// On app_main, once the bus is up and the station set up (the default event
// loop exists), before its radio starts: the RTC's alarm taken back -- a
// reset in a sync leaves it armed and INT low -- the INT's interrupt, and
// the wait for Wi-Fi's first address. HAL_ERR_IO when the address cannot be
// waited for: no time from the network, the rest runs. An INT that does not
// interrupt is logged; the RTC is then written without being measured. Once.
hal_status_t net_time_init(const net_time_config_t *cfg);

// event_loop.h's step hook: what is due, on the event loop's task, and when
// next. RTC_SYNC_NO_DEADLINE when nothing is.
uint64_t net_time_step(void *ctx, uint64_t now_ms);

// Something other than this module wrote the RTC -- time_set, rtc_stop: a
// sync under way stops, its alarm taken back, and the last write's mark no
// longer says where the RTC stood. From any task; nothing when not
// initialised.
void net_time_forget_rtc_mark(void);

// What the console's `time` prints, as the event loop last left it.
typedef struct {
    bool started;       // net_time_init() succeeded
    bool sntp_running;  // Wi-Fi had an address, and SNTP was set going
    int32_t sntp_err;   // esp_netif_sntp_init()'s esp_err_t
    const char *server;
    uint32_t syncs;
    uint32_t refused;       // answers outside the years believed
    uint64_t last_sync_ms;  // port_clock's now_ms() at the last answer; 0 before one
    uint64_t last_sync_utc_ms;
    bool step_known;  // the system time had a time to step from
    int64_t system_minus_sntp_ms;
    rtc_sync_phase_t phase;
    uint32_t int_edges;
    bool int_low;
    bool offset_known;  // the RTC's tick found before the last write
    int64_t rtc_minus_sntp_ms;
    uint32_t offset_uncertainty_ms;
    bool drift_known;
    float drift_ppm;
    float drift_uncertainty_ppm;
    uint32_t writes;
    uint32_t write_failures;
    bool landed_known;
    int64_t residual_ms;     // where the last write's tick landed
    uint32_t first_tick_ms;  // the delay the next write is planned with
    bool first_tick_learned;
} net_time_status_t;

void net_time_status(net_time_status_t *out);

#ifdef __cplusplus
}
#endif

#endif  // HAC_APPS_FIRMWARE_ESP32_NET_TIME_H
