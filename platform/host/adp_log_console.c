// SPDX-License-Identifier: Apache-2.0

#include "adp_log_console.h"

#include <inttypes.h>
#include <stdio.h>

static const char *level_name(log_level_t level)
{
    switch (level) {
        case LOG_LEVEL_ERROR:
            return "ERROR";
        case LOG_LEVEL_WARN:
            return "WARN";
        case LOG_LEVEL_INFO:
            return "INFO";
        case LOG_LEVEL_DEBUG:
            return "DEBUG";
        default:
            return "?";
    }
}

static void print_field(FILE *out, const log_field_t *f)
{
    const char *key = (f->key != NULL) ? f->key : "?";

    switch (f->kind) {
        case LOG_FIELD_I64:
            fprintf(out, " %s=%" PRId64, key, f->v.i64);
            return;
        case LOG_FIELD_U64:
            fprintf(out, " %s=%" PRIu64, key, f->v.u64);
            return;
        case LOG_FIELD_F32:
            fprintf(out, " %s=%g", key, (double)f->v.f32);
            return;
        case LOG_FIELD_BOOL:
            fprintf(out, " %s=%s", key, f->v.b ? "true" : "false");
            return;
        case LOG_FIELD_STR:
            fprintf(out, " %s=%s", key, (f->v.str != NULL) ? f->v.str : "(null)");
            return;
        default:
            fprintf(out, " %s=?", key);
            return;
    }
}

static void console_write(const port_log_t *port, log_level_t level, const char *tag,
                          const char *msg, const log_field_t *fields, size_t field_count)
{
    (void)port;  // no per-instance state to read: every adp_log_console_t behaves identically

    FILE *out = (level == LOG_LEVEL_ERROR || level == LOG_LEVEL_WARN) ? stderr : stdout;

    fprintf(out, "[%s] %s: %s", level_name(level), (tag != NULL) ? tag : "-",
            (msg != NULL) ? msg : "");

    if (fields != NULL) {
        for (size_t i = 0u; i < field_count; ++i) {
            print_field(out, &fields[i]);
        }
    }
    fputc('\n', out);
}

void adp_log_console_init(adp_log_console_t *l)
{
    l->port.write = console_write;
    l->port.impl = l;
}

const port_log_t *adp_log_console_port(const adp_log_console_t *l)
{
    return &l->port;
}
