// SPDX-License-Identifier: Apache-2.0

#include "port_log_contract.h"

#include <string.h>

// Longer than any fixed-size line buffer an adapter is likely to use
// (ESP_LOG's default line buffer and a typical host printf line are both
// well under 1 KiB), to see whether an over-long message is truncated safely
// rather than overflowing something.
#define CONTRACT_LONG_MSG_LEN 600u

static void check(contract_report_fn report, void *ctx, unsigned *failures, const char *name,
                  bool ok)
{
    report(ctx, name, ok);
    if (!ok) {
        (*failures)++;
    }
}

// Every check below asserts the same thing: the call returned instead of
// crashing. That is deliberately the whole contract for a port whose job is
// a side effect -- see port_log_contract.h. Passing `true` after a call
// records that this line was reached; a real fault (a NULL dereference
// inside the adapter, a buffer overrun) ends the process before `check()`
// ever runs, which `report` cannot observe but a crashed contract run makes
// obvious to whoever is reading its output.
unsigned port_log_contract_run(const port_log_t *log, contract_report_fn report, void *report_ctx)
{
    unsigned failures = 0u;

    check(report, report_ctx, &failures, "log.vtable.write", log->write != NULL);
    if (log->write == NULL) {
        return failures;
    }

    log->write(log, LOG_LEVEL_ERROR, "contract", "error level", NULL, 0u);
    check(report, report_ctx, &failures, "log.level.error", true);

    log->write(log, LOG_LEVEL_WARN, "contract", "warn level", NULL, 0u);
    check(report, report_ctx, &failures, "log.level.warn", true);

    log->write(log, LOG_LEVEL_INFO, "contract", "info level", NULL, 0u);
    check(report, report_ctx, &failures, "log.level.info", true);

    log->write(log, LOG_LEVEL_DEBUG, "contract", "debug level", NULL, 0u);
    check(report, report_ctx, &failures, "log.level.debug", true);

    log->write(log, LOG_LEVEL_INFO, NULL, "null tag", NULL, 0u);
    check(report, report_ctx, &failures, "log.tag.null_tolerated", true);

    log->write(log, LOG_LEVEL_INFO, "contract", NULL, NULL, 0u);
    check(report, report_ctx, &failures, "log.msg.null_tolerated", true);

    // Every field kind, in one call, up to LOG_FIELDS_MAX -- a real call site
    // (adp_display_null's flush log) uses this many at once.
    const log_field_t fields[LOG_FIELDS_MAX] = {
        log_i64("i64", -7),     log_u64("u64", 42u),     log_f32("f32", 1.5f),
        log_bool("bool", true), log_str("str", "value"), log_str("str_null", NULL),
    };
    log->write(log, LOG_LEVEL_INFO, "contract", "all field kinds", fields, LOG_FIELDS_MAX);
    check(report, report_ctx, &failures, "log.fields.every_kind_and_full_array", true);

    const log_field_t unnamed = log_u64(NULL, 1u);
    log->write(log, LOG_LEVEL_INFO, "contract", "unnamed field", &unnamed, 1u);
    check(report, report_ctx, &failures, "log.field.null_key_tolerated", true);

    char long_msg[CONTRACT_LONG_MSG_LEN + 1u];
    memset(long_msg, 'x', CONTRACT_LONG_MSG_LEN);
    long_msg[CONTRACT_LONG_MSG_LEN] = '\0';
    log->write(log, LOG_LEVEL_INFO, "contract", long_msg, NULL, 0u);
    check(report, report_ctx, &failures, "log.msg.over_long_does_not_crash", true);

    return failures;
}
