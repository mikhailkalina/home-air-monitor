// SPDX-License-Identifier: Apache-2.0
//
// Composition root for the ESP32-S3 firmware (docs/architecture.md 6.2 wires
// adapters here). Phase 2a brought the board up with no panel driven and no
// sensors attached; phase 2b (docs/adr/0007-epaper-backend.md) adds the real
// display -- EPDiy driving the ED047TC1 through its row-by-row i80 engine,
// behind platform/esp32_t5s3/adp_display.c. No I2C, no sensors yet.
//
// What runs: adp_log and adp_clock, the real display adapter (or the null
// one, CONFIG_HAC_DISPLAY_NULL -- see main/Kconfig.projbuild), the
// port_clock and port_log contract suites run once against the real
// adapters (docs/adr/0005), optionally the whole-panel bring-up diagnostic
// (CONFIG_HAC_EPD_TEST_PATTERN, which costs the panel's minimum
// full-refresh interval before the first rendered frame -- see
// run_epd_test_pattern below), and the FreeRTOS pump from event_loop.c
// driving update_policy against bringup_reading.c's synthetic data.
//
// CONFIG_HAC_EPD_CALIBRATION (phase 2c, off by default) replaces the pump
// with epd_calibration.c's measurement run: the same adapters, no event loop.

#include <stdint.h>

#include "esp_heap_caps.h"

#include "adp_clock.h"
#include "adp_display.h"
#include "adp_display_null.h"
#include "adp_log.h"
#include "board_config.h"

#include "app/update_policy.h"
#include "hal_status.h"
#include "port_clock.h"
#include "port_display.h"
#include "port_log.h"

#include "port_clock_contract.h"
#include "port_log_contract.h"

#include "bringup_reading.h"
#include "epd_calibration.h"
#include "event_loop.h"

// Arithmetic behind this figure: ~2 KB IDF/vsnprintf baseline + ~400 B
// view_model_t + ~400 B air_reading_t + ~500 B through the render call chain
// is roughly 3.3 KB at peak, doubled for margin. event_loop.c logs
// uxTaskGetStackHighWaterMark() every iteration, so this number gets
// revisited from evidence rather than kept out of superstition.
#define BRINGUP_TASK_STACK_BYTES 6144u

// Off the protocol core (0), forward-looking to Wi-Fi joining this task's
// core in phase 3.
#define BRINGUP_TASK_PRIORITY 5
#define BRINGUP_TASK_CORE 1

typedef struct {
    const port_log_t *log;
    const char *tag;
} contract_ctx_t;

static void contract_report(void *ctx_v, const char *check, bool ok)
{
    const contract_ctx_t *ctx = (const contract_ctx_t *)ctx_v;
    const log_field_t fields[] = {log_bool("ok", ok)};
    port_log_write(ctx->log, ok ? LOG_LEVEL_DEBUG : LOG_LEVEL_ERROR, ctx->tag, check, fields, 1u);
}

#if CONFIG_HAC_EPD_TEST_PATTERN
// Bring-up diagnostic (phase 2b/2c, CONFIG_HAC_EPD_TEST_PATTERN -- see
// main/Kconfig.projbuild), run once at startup before the event loop: fill the
// framebuffer solid black and flush it as a full refresh. On this backend that
// is epd_clear() -- the panel visibly cycles to white -- followed by a
// MODE_GC16 redraw of the buffer, so the panel should settle on solid black. That isolates "does
// the panel respond to a drive sequence at all" from "is screen_home's rendering correct": a panel
// that stays blank through this is a driver or wiring problem, while one that
// goes black has been driven correctly and any later wrong image is a
// rendering problem instead. Off by default since phase 2c's calibration run
// drove this panel through every refresh mode; kept for bringing up another
// unit.
//
// Deliberately REFRESH_FULL rather than REFRESH_CLEAR, which is what this
// used while FastEPD was the backend. Under EPDiy, REFRESH_CLEAR is
// epd_clear() alone: a dedicated multi-cycle drive to white that ignores the
// framebuffer by design, which would make a black fill meaningless. Going
// through the ordinary full-refresh path also means this probe exercises the
// same mapping every later frame uses, rather than a path of its own.
//
// ONE whole-panel drive, and then a wait.
// port_display_t::min_full_refresh_interval_ms is what this adapter reports
// to the core as the shortest time the panel tolerates between whole-panel
// refreshes, and core/app/update_policy.c defers every later refresh to
// honour it -- so a boot-time diagnostic that issued two of them two seconds
// apart, and then handed over to a policy that immediately issued a third,
// would be the one place in the firmware contradicting a limit the rest of
// the code treats as the panel's own. The light direction is proved by the
// event loop's own first frame: screen_home renders on a white ground.
static void run_epd_test_pattern(port_display_t *display, const port_clock_t *clock,
                                 const port_log_t *log)
{
    framebuffer_t *fb = display->get_framebuffer(display);
    if (fb == NULL) {
        port_log_write(log, LOG_LEVEL_ERROR, "main", "test_pattern: get_framebuffer returned NULL",
                       NULL, 0u);
        return;
    }

    framebuffer_clear(fb, 0x00u);  // VM_INK_PRIMARY -- black
    const hal_status_t status = display->flush(display, NULL, REFRESH_FULL);
    const log_field_t fields[] = {log_i64("status", (int64_t)status)};
    port_log_write(log, LOG_LEVEL_INFO, "main",
                   "test_pattern: solid black flushed (REFRESH_FULL, whole panel)", fields, 1u);

    // update_policy_init() starts with have_full_refresh = false, so the
    // policy's first frame is due immediately and it cannot space itself
    // from a refresh it never saw. Waiting here is what keeps that first
    // frame outside the interval this diagnostic just consumed. The delay is
    // the declared constant, not a shorter convenience value: the point is
    // to honour the limit, not to approximate it.
    const log_field_t wait_fields[] = {
        log_u64("wait_ms", BOARD_EPD_MIN_FULL_REFRESH_INTERVAL_MS),
    };
    port_log_write(log, LOG_LEVEL_INFO, "main",
                   "test_pattern: waiting out the panel's minimum full-refresh interval before "
                   "the first rendered frame",
                   wait_fields, sizeof(wait_fields) / sizeof(wait_fields[0]));
    clock->delay_ms(clock, BOARD_EPD_MIN_FULL_REFRESH_INTERVAL_MS);
}
#endif  // CONFIG_HAC_EPD_TEST_PATTERN

// That memory budget is what every phase from 2b onward has to fit inside --
// see the phase-2a plan's "Deliverable" section.
static void log_memory_budget(const port_log_t *log)
{
    const size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t largest_psram_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    const log_field_t fields[] = {
        log_u64("free_heap_bytes", free_heap),
        log_u64("free_internal_bytes", free_internal),
        log_u64("free_psram_bytes", free_psram),
        log_u64("largest_free_psram_block_bytes", largest_psram_block),
    };
    port_log_write(log, LOG_LEVEL_INFO, "main", "memory budget at startup", fields,
                   sizeof(fields) / sizeof(fields[0]));
}

// Runs port_clock_contract_run()/port_log_contract_run() against the real
// adapters this firmware actually uses, at boot, because there is no
// idf.py -T test-app scaffold yet (docs/adr/0005-contract-suites-run-at-boot.md).
// The bodies are the exact object code tests/contract/test_port_clock_contract.c
// and test_port_log_contract.c exercise on the host: same source, two
// runners, via cmake/sources_contract.cmake.
static void run_contract_suites(const port_clock_t *clock, const port_log_t *log)
{
    contract_ctx_t clock_ctx = {.log = log, .tag = "contract.port_clock"};
    const unsigned clock_failures = port_clock_contract_run(clock, contract_report, &clock_ctx);

    contract_ctx_t log_ctx = {.log = log, .tag = "contract.port_log"};
    const unsigned log_failures = port_log_contract_run(log, contract_report, &log_ctx);

    const log_field_t tally_fields[] = {
        log_u64("port_clock_failures", clock_failures),
        log_u64("port_log_failures", log_failures),
    };
    const log_level_t level =
        (clock_failures == 0u && log_failures == 0u) ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR;
    port_log_write(log, level, "contract", "contract suites complete", tally_fields,
                   sizeof(tally_fields) / sizeof(tally_fields[0]));
}

void app_main(void)
{
    static adp_log_t s_log;
    static adp_clock_t s_clock;
#if CONFIG_HAC_DISPLAY_NULL
    static adp_display_null_t s_display;
#else
    static adp_display_t s_display;
#endif
    static update_policy_t s_policy;

    adp_log_init(&s_log);
    const port_log_t *log = adp_log_port(&s_log);

    adp_clock_init(&s_clock, log);
    const port_clock_t *clock = adp_clock_port(&s_clock);

    port_log_write(log, LOG_LEVEL_INFO, "main",
#if CONFIG_HAC_DISPLAY_NULL
                   "phase 2b: board bring-up -- null display (CONFIG_HAC_DISPLAY_NULL), no sensors",
#elif CONFIG_HAC_EPD_CALIBRATION
                   "phase 2c: panel calibration mode (CONFIG_HAC_EPD_CALIBRATION), no event loop",
#else
                   "phase 2b: board bring-up -- EPDiy driving the panel, no sensors",
#endif
                   NULL, 0u);

    log_memory_budget(log);
    run_contract_suites(clock, log);

#if CONFIG_HAC_DISPLAY_NULL
    const hal_status_t display_status =
        adp_display_null_init(&s_display, BOARD_EPD_WIDTH, BOARD_EPD_HEIGHT, log);
    if (display_status != HAL_OK) {
        const log_field_t fields[] = {log_i64("status", (int64_t)display_status)};
        port_log_write(log, LOG_LEVEL_ERROR, "main", "adp_display_null_init failed", fields, 1u);
        return;  // nothing else can run without a framebuffer
    }
    port_display_t *display = adp_display_null_port(&s_display);
#else
    const hal_status_t display_status =
        adp_display_init(&s_display, BOARD_EPD_WIDTH, BOARD_EPD_HEIGHT, log);
    if (display_status != HAL_OK) {
        const log_field_t fields[] = {log_i64("status", (int64_t)display_status)};
        port_log_write(log, LOG_LEVEL_ERROR, "main", "adp_display_init failed", fields, 1u);
        return;  // nothing else can run without a framebuffer
    }
    port_display_t *display = adp_display_port(&s_display);
#if CONFIG_HAC_EPD_TEST_PATTERN
    run_epd_test_pattern(display, clock, log);
#endif
#if CONFIG_HAC_EPD_CALIBRATION
    const epd_calibration_config_t calibration = {
        .iterations = (uint32_t)CONFIG_HAC_EPD_CALIBRATION_ITERATIONS,
#if CONFIG_HAC_EPD_CALIBRATION_GHOST_SHIFT
        .ghost_pattern = EPD_CAL_GHOST_SHIFT,
#else
        .ghost_pattern = EPD_CAL_GHOST_SAWTOOTH,
#endif
        .ghost_partials = (uint32_t)CONFIG_HAC_EPD_CALIBRATION_GHOST_PARTIALS,
        .ghost_pause_ms = (uint32_t)CONFIG_HAC_EPD_CALIBRATION_GHOST_PAUSE_MS,
        .residue_normal_fulls = (uint32_t)CONFIG_HAC_EPD_CALIBRATION_RESIDUE_NORMAL_FULLS,
        .residue_deep_events = (uint32_t)CONFIG_HAC_EPD_CALIBRATION_RESIDUE_DEEP_EVENTS,
        .residue_extra_clears = (uint32_t)CONFIG_HAC_EPD_CALIBRATION_RESIDUE_EXTRA_CLEARS,
    };
    epd_calibration_run(display, clock, log, &calibration);
    return;  // the calibration run replaces the event loop; the panel keeps its last image
#endif
#endif

    const update_policy_config_t cfg = update_policy_config_default();
    const update_policy_limits_t limits = update_policy_limits_from_display(display);
    update_policy_init(&s_policy, clock, &cfg, &limits);

    const log_field_t panel_fields[] = {
        log_u64("width", display->width),
        log_u64("height", display->height),
        log_u64("min_full_refresh_interval_ms", limits.min_full_refresh_interval_ms),
        log_u64("max_partial_refreshes_before_full", limits.max_partial_refreshes_before_full),
    };
    port_log_write(log, LOG_LEVEL_INFO, "main", "panel and policy limits", panel_fields,
                   sizeof(panel_fields) / sizeof(panel_fields[0]));

    event_loop_deps_t deps = {
        .clock = clock,
        .display = display,
        .log = log,
        .policy = &s_policy,
        .read_reading = bringup_reading_update,
        .read_reading_ctx = NULL,
        .max_reading_age_ms = BRINGUP_MAX_READING_AGE_MS,
    };
    event_loop_start(&deps, BRINGUP_TASK_STACK_BYTES, BRINGUP_TASK_PRIORITY, BRINGUP_TASK_CORE);
}
