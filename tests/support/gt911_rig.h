// SPDX-License-Identifier: Apache-2.0
//
// A GT911 on the fake bus, for the tests that drive the driver against the
// model (test_gt911.c, test_gt911_input.c, the port_input contract): a
// virtual clock, the bus, the model at an address, an INT line whose three
// operations reach the model's pin -- a pulse moving the clock on by its
// length, as the board's busy-wait does -- and the model's interrupt
// signalling the port_input implementation, as the board's ISR does.
//
// Header-only: every user is one test executable, and each links what it
// needs (hac_drivers, hac_platform_host).

#ifndef HAC_TESTS_SUPPORT_GT911_RIG_H
#define HAC_TESTS_SUPPORT_GT911_RIG_H

#include <stdint.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_i2c_fake.h"
#include "gt911.h"
#include "gt911_input.h"
#include "i2c_models/gt911_model.h"

// The GT911's two addresses (its datasheet, section 6: 0xBA and 0x28 as
// 8-bit addresses). The part's, not board_config.h's.
#define GT911_RIG_ADDR 0x5Du
#define GT911_RIG_ALT_ADDR 0x14u

// The panel the rig maps onto, and the map that makes the model's portrait
// frame landscape -- the vendor's own swap and mirror for this board.
#define GT911_RIG_PANEL_W 960u
#define GT911_RIG_PANEL_H 540u

typedef struct {
    adp_clock_virtual_t clock;
    adp_i2c_fake_t bus;
    gt911_model_t model;
    gt911_int_line_t line;
    gt911_input_t input;
    bool input_set_up;
    unsigned interrupts;
} gt911_rig_t;

static inline void gt911_rig_drive_low(void *ctx)
{
    gt911_rig_t *r = (gt911_rig_t *)ctx;
    gt911_model_int_drive_low(&r->model, r->clock.now_ms);
}

static inline void gt911_rig_pulse_high(void *ctx, uint32_t us)
{
    gt911_rig_t *r = (gt911_rig_t *)ctx;
    gt911_model_int_pulse_high(&r->model, us, r->clock.now_ms);
    adp_clock_virtual_advance(&r->clock, (us + 999u) / 1000u);
}

static inline void gt911_rig_release(void *ctx)
{
    gt911_rig_t *r = (gt911_rig_t *)ctx;
    gt911_model_int_release(&r->model, r->clock.now_ms);
}

static inline void gt911_rig_interrupt(void *ctx)
{
    gt911_rig_t *r = (gt911_rig_t *)ctx;
    r->interrupts++;
    if (r->input_set_up) {
        gt911_input_signal(&r->input);
    }
}

// The model at `address` (0 leaves the bus empty), awake and untouched.
static inline void gt911_rig_init(gt911_rig_t *r, uint8_t address)
{
    memset(r, 0, sizeof(*r));
    adp_clock_virtual_init(&r->clock, 1.0);
    adp_clock_virtual_advance(&r->clock, 1000u);  // a boot's worth: nothing is at t = 0
    adp_i2c_fake_init(&r->bus, &r->clock);
    gt911_model_init(&r->model);
    gt911_model_set_interrupt(&r->model, gt911_rig_interrupt, r);
    if (address != 0u) {
        (void)adp_i2c_fake_attach(&r->bus, address, gt911_model_port(&r->model));
    }
    r->line.drive_low = gt911_rig_drive_low;
    r->line.pulse_high = gt911_rig_pulse_high;
    r->line.release = gt911_rig_release;
    r->line.ctx = r;
}

static inline gt911_config_t gt911_rig_device_config(gt911_rig_t *r)
{
    gt911_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bus = adp_i2c_fake_port(&r->bus);
    cfg.clock = adp_clock_virtual_port(&r->clock);
    cfg.log = NULL;
    cfg.address = GT911_RIG_ADDR;
    cfg.alt_address = GT911_RIG_ALT_ADDR;
    cfg.int_line = &r->line;
    return cfg;
}

static inline gt911_touch_map_t gt911_rig_map(void)
{
    gt911_touch_map_t map;
    memset(&map, 0, sizeof(map));
    map.swap_xy = true;
    map.mirror_x = false;
    map.mirror_y = true;
    map.panel_width = GT911_RIG_PANEL_W;
    map.panel_height = GT911_RIG_PANEL_H;
    return map;
}

// gt911_input over the model, set up; not initialised.
static inline hal_status_t gt911_rig_input_setup(gt911_rig_t *r)
{
    gt911_input_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.device = gt911_rig_device_config(r);
    cfg.map = gt911_rig_map();
    cfg.move_threshold_px = GT911_INPUT_MOVE_THRESHOLD_PX;
    cfg.log = NULL;
    const hal_status_t st = gt911_input_setup(&r->input, &cfg);
    r->input_set_up = (st == HAL_OK);
    return st;
}

// One finger at (x, y) in the model's own frame, track `id`.
static inline gt911_model_point_t gt911_rig_point(uint8_t id, uint16_t x, uint16_t y)
{
    gt911_model_point_t p;
    p.track_id = id;
    p.x = x;
    p.y = y;
    p.size = 20u;
    return p;
}

#endif  // HAC_TESTS_SUPPORT_GT911_RIG_H
