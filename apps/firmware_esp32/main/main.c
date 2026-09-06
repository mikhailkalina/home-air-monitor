// SPDX-License-Identifier: Apache-2.0
//
// Composition root for the ESP32-S3 firmware -- phase 2a, board bring-up
// with no panel driven and no sensors attached (docs/architecture.md 6.2
// wires adapters here; this is that wiring for the adapters this phase has).
//
// What runs: adp_log and adp_clock, a null display that reports the real
// panel geometry and allocates the real PSRAM framebuffer, the port_clock
// and port_log contract suites run once against those two real adapters
// (docs/adr/0005), and the FreeRTOS pump from event_loop.c driving
// update_policy against bringup_reading.c's synthetic data. No I2C, no
// drivers, no real display -- see the phase-2a plan for what is deliberately
// out of scope.

#include <stdint.h>

#include "esp_heap_caps.h"

#include "adp_clock.h"
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
    static adp_display_null_t s_display;
    static update_policy_t s_policy;

    adp_log_init(&s_log);
    const port_log_t *log = adp_log_port(&s_log);

    adp_clock_init(&s_clock);
    const port_clock_t *clock = adp_clock_port(&s_clock);

    port_log_write(log, LOG_LEVEL_INFO, "main",
                   "phase 2a: board bring-up -- no panel driven, no sensors", NULL, 0u);

    log_memory_budget(log);
    run_contract_suites(clock, log);

    const hal_status_t display_status =
        adp_display_null_init(&s_display, BOARD_EPD_WIDTH, BOARD_EPD_HEIGHT, log);
    if (display_status != HAL_OK) {
        const log_field_t fields[] = {log_i64("status", (int64_t)display_status)};
        port_log_write(log, LOG_LEVEL_ERROR, "main", "adp_display_null_init failed", fields, 1u);
        return;  // nothing else can run without a framebuffer
    }
    port_display_t *display = adp_display_null_port(&s_display);

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
