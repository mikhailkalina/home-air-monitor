// SPDX-License-Identifier: Apache-2.0
//
// The Goodix GT911 touch controller over port_i2c: find it, name it, read
// what it reports, put it to sleep and wake it. Plain functions, as the
// PCF8563's are; the port_input implementation over them is gt911_input.h
// (docs/adr/0025-touch-input-gt911.md).
//
// Written from Goodix's documents, not from anyone's driver -- in particular
// not from Xinyuan-LilyGO/LilyGo-EPD47, which is GPL-3.0 and read for facts
// about the board only (docs/adr/0007-epaper-backend.md):
//
//   * the GT911 datasheet, Rev.10 (2015-06-25), as Waveshare hosts it: the
//     I2C transfers (a 16-bit register address, high byte first, then data,
//     the pointer advancing after every byte, section 6), the two addresses
//     latched at power-up (0xBA/0xBB and 0x28/0x29 -- 0x5D and 0x14 as 7-bit
//     addresses), the interrupt -- a pulse in every scan cycle while touched,
//     on the edge the configuration selects (8.2) -- and sleep: "requires INT
//     output low before the command", woken by "a high level to INT pin for
//     2ms to 5ms", the interval between the two "longer than 58ms" (8.1 d);
//   * Goodix's GT911 programming guide for the register map, which that
//     datasheet does not carry: the command register 0x8040 (5: screen off,
//     i.e. sleep), the configuration area 0x8047-0x8100, the product ID at
//     0x8140, the status register 0x814E and the point records after it. No
//     legible copy of the guide was at hand when this was written; the map
//     below is the one every GT9xx driver reads, and the board checks it --
//     a product ID of "911" read back at 0x8140 is the map's own proof
//     (docs/hardware/board_notes.md, open item "Touch controller part
//     number").
//
// The configuration area is the vendor's calibration for this panel --
// sensor channels, thresholds, the resolution, the interrupt's edge. This
// driver reads it and NEVER writes it: there is no function here that could,
// and tests/unit/test_gt911.c holds every call to that against a model that
// fails any write into the area. A wrong configuration written to a GT911 is
// kept in its flash and survives power cycles.
//
// What the board does to the part (docs/hardware/board_notes.md, "Pin
// assignments", "Connectors"): it runs from VDD3V3, which firmware cannot
// switch, and its reset is a pull-up with no line to the ESP32. So nothing
// resets it but a power cycle, its address is whatever it latched at
// power-up (0x5D seen, 0x14 the other), and a GT911 a previous firmware put
// to sleep is still asleep after an ESP32 reset. gt911_init() therefore
// wakes it before anything else.
//
// INT is two things: the controller's interrupt output, and the host's way
// to wake it. The driver drives it only through gt911_int_line_t -- the
// platform's GPIO on the board, a device model's pin on the host -- and only
// to sleep and wake; the interrupt itself is the caller's (gt911_input.h).

#ifndef HAC_DRIVERS_GT911_GT911_H
#define HAC_DRIVERS_GT911_GT911_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"
#include "port_clock.h"
#include "port_i2c.h"
#include "port_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- the register map (programming guide; see above) --------------------------

#define GT911_REG_COMMAND 0x8040u  // write: 5 = screen off (sleep)
#define GT911_CMD_SCREEN_OFF 0x05u

// The configuration area: Config_Version at its start, then X and Y output
// maximum (little-endian), the touch number, Module_Switch1 -- whose bits 1:0
// select the interrupt's trigger -- and the rest of the vendor's calibration,
// Config_Chksum at 0x80FF and Config_Fresh at 0x8100.
#define GT911_REG_CONFIG_FIRST 0x8047u
#define GT911_REG_CONFIG_CHECKSUM 0x80FFu
#define GT911_REG_CONFIG_LAST 0x8100u
#define GT911_CONFIG_LEN (GT911_REG_CONFIG_LAST - GT911_REG_CONFIG_FIRST + 1u)  // 186

// Offsets into the configuration area.
#define GT911_CONFIG_VERSION 0u
#define GT911_CONFIG_X_MAX 1u  // 2 bytes, low first
#define GT911_CONFIG_Y_MAX 3u  // 2 bytes, low first
#define GT911_CONFIG_TOUCH_NUMBER 5u
#define GT911_CONFIG_MODULE_SWITCH1 6u
#define GT911_CONFIG_TRIGGER_MASK 0x03u
#define GT911_CONFIG_TOUCH_NUMBER_MASK 0x0Fu

// Product information, read-only: the ID in ASCII ("911", NUL-padded), the
// firmware version and the resolution in use, little-endian, the vendor id.
#define GT911_REG_PRODUCT_ID 0x8140u
#define GT911_PRODUCT_INFO_LEN 11u
#define GT911_PRODUCT_ID_LEN 4u
#define GT911_PRODUCT_ID "911"

// The status register: bit 7 a report is ready for the host, bits 3:0 how
// many points it holds. The host writes 0 once it has read the report; until
// then the controller does not replace it.
#define GT911_REG_STATUS 0x814Eu
#define GT911_STATUS_BUFFER_READY 0x80u
#define GT911_STATUS_COUNT_MASK 0x0Fu

// One record per point from 0x814F: the track ID, X, Y and size
// (little-endian), and a reserved byte.
#define GT911_REG_POINTS 0x814Fu
#define GT911_POINT_RECORD_LEN 8u
#define GT911_MAX_POINTS 5u

// --- timing ---------------------------------------------------------------------

// The datasheet's wake pulse is 2 to 5 ms; the middle of it. The platform
// times it (gt911_int_line_t::pulse_high), not port_clock: delay_ms() may
// round a 3 ms wait up past 5 ms on the board's 10 ms tick (ADR 0006).
#define GT911_WAKE_PULSE_US 3000u

// The datasheet: from the sleep command to a wake pulse, longer than 58 ms.
#define GT911_SLEEP_TO_WAKE_MS 58u

// ESTIMATE: how long after a wake pulse before the first transfer. The
// datasheet gives no figure; 50 ms is how long Linux's driver holds the
// line after one. The board's first wake replaces it (the boot log's `gt911:
// found` line says how many attempts it took).
#define GT911_WAKE_SETTLE_MS 50u

// CHOICE: how long init() keeps probing for the controller -- both
// addresses in turn, address-only probes, which cost no error lines -- before
// it gives up. The vendor's own example waits a whole second after waking
// the part from its sleep "otherwise the touch device cannot be addressed"
// (examples/touch/touch.ino, read for that fact only), so a second, asked
// every GT911_ANSWER_RETRY_MS rather than waited out.
#define GT911_ANSWER_TIMEOUT_MS 1000u
#define GT911_ANSWER_RETRY_MS 20u

// CHOICE: one transfer's deadline. The longest, the configuration area, is
// 188 bytes -- about 17 ms at the board's 100 kHz.
#define GT911_TRANSFER_TIMEOUT_MS 50u

// --- the INT line -----------------------------------------------------------------

// What the driver asks of the INT line. Every function is required.
typedef struct {
    // Drive INT low and keep it there: before the sleep command, and through
    // the sleep that follows.
    void (*drive_low)(void *ctx);
    // Drive INT high for `us` microseconds, then release it to the
    // controller (an input again). Timed by the platform, which is the only
    // one that can keep it inside the datasheet's 2-5 ms.
    void (*pulse_high)(void *ctx, uint32_t us);
    // Release INT: an input, driven by the controller.
    void (*release)(void *ctx);
    void *ctx;
} gt911_int_line_t;

// The interrupt's trigger, Module_Switch1 bits 1:0.
typedef enum {
    GT911_TRIGGER_RISING = 0,
    GT911_TRIGGER_FALLING,
    GT911_TRIGGER_LOW_LEVEL,
    GT911_TRIGGER_HIGH_LEVEL,
} gt911_trigger_t;

typedef struct {
    port_i2c_t *bus;
    const port_clock_t *clock;
    const port_log_t *log;  // may be NULL
    uint8_t address;        // tried first
    uint8_t alt_address;    // tried when `address` never answers; 0 for none
    // NULL: the controller cannot be woken or put to sleep, and init() does
    // not try -- a GT911 left asleep is then not found.
    const gt911_int_line_t *int_line;
} gt911_config_t;

// What the controller says about itself.
typedef struct {
    uint8_t address;                            // the one that answered
    char product_id[GT911_PRODUCT_ID_LEN + 1];  // "911"
    uint16_t firmware_version;
    uint16_t x_resolution;  // 0x8146, what it reports in now
    uint16_t y_resolution;
    uint8_t vendor_id;
    // The vendor's configuration, as read.
    uint8_t config_version;
    uint16_t x_max;
    uint16_t y_max;
    uint8_t touch_number;
    uint8_t module_switch1;
    gt911_trigger_t trigger;
    bool config_checksum_ok;  // Config_Chksum matches the bytes before it
    uint8_t config_checksum;
} gt911_info_t;

typedef struct {
    uint8_t track_id;
    uint16_t x;  // in the controller's own frame, 0..x_max
    uint16_t y;
    uint16_t size;
} gt911_point_t;

// One report.
typedef struct {
    bool ready;      // the status register's buffer-ready bit was set
    uint8_t count;   // points in `points`
    uint8_t status;  // the status register as read
    gt911_point_t points[GT911_MAX_POINTS];
} gt911_frame_t;

typedef struct {
    gt911_config_t cfg;
    bool found;
    bool asleep;
    bool slept_at_known;
    uint64_t slept_at_ms;
    uint32_t answer_attempts;  // what the last init() or wake() needed
    gt911_info_t info;
} gt911_t;

// Copies `cfg`; no bus traffic. HAL_ERR_INVALID_ARG without a bus, a clock
// or an address.
hal_status_t gt911_setup(gt911_t *t, const gt911_config_t *cfg);

// Wakes the controller (when there is an INT line), finds it -- `address`
// and `alt_address` probed in turn, for up to GT911_ANSWER_TIMEOUT_MS, by
// address alone, so a silent address costs no failed data transfer -- reads
// and checks its product ID, reads the configuration, and clears any report
// left waiting. May be called again at any time: the address it latched can
// change under a power glitch the ESP32 never sees.
//
//   HAL_OK             found, a GT911
//   HAL_ERR_NOT_FOUND  nothing answered at either address
//   HAL_ERR_UNSUPPORTED  something answered, but its product ID is not "911"
//   else               the failing transfer's status
hal_status_t gt911_init(gt911_t *t);

// Reads the status register and, when a report is ready, its points, then
// clears it. `out->ready` false is a read that found nothing -- an
// interrupt that came early, or none at all.
hal_status_t gt911_read_frame(gt911_t *t, gt911_frame_t *out);

// INT low, then the sleep command. Nothing answers until gt911_wake().
// HAL_ERR_UNSUPPORTED without an INT line: it could never be woken.
hal_status_t gt911_sleep(gt911_t *t);

// Waits out GT911_SLEEP_TO_WAKE_MS since the sleep command, pulses INT
// high, and waits for the controller to answer again. HAL_ERR_UNSUPPORTED
// without an INT line.
hal_status_t gt911_wake(gt911_t *t);

const gt911_info_t *gt911_info(const gt911_t *t);

// "rising", "falling", "low", "high"; never NULL.
const char *gt911_trigger_name(gt911_trigger_t trigger);

// What Config_Chksum must be for `config` (GT911_CONFIG_LEN bytes): the two's
// complement of the sum of the bytes before it.
uint8_t gt911_config_checksum(const uint8_t *config);

#ifdef __cplusplus
}
#endif

#endif  // HAC_DRIVERS_GT911_GT911_H
