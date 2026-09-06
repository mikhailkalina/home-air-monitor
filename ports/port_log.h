// SPDX-License-Identifier: Apache-2.0
//
// The logging port.
//
// core/ has no stdio.h, so no signature here can ask its caller to format a
// string. What the core actually needs to say is a short message plus a
// handful of named scalars: look at what core/app/update_policy.c already
// hands its caller today, through update_policy_action_name() and
// update_policy_reason_name() (core/app/update_policy.h) plus the plain
// fields of update_decision_t -- an action, a reason, a partial-refresh
// count, a couple of booleans, a deadline. The app_core designed in
// docs/architecture.md 6.1 wants the same shape for its event log: an event
// kind, next_deadline_ms, allow_sleep, a sensor id. Not one of those is
// prose. port_log carries exactly that: structured fields the adapter
// renders however it likes -- one ESP_LOGI format string on the device, one
// line per field on the host -- so the core can log without ever holding a
// format string or a va_list.
//
// A caller that does have stdio (an adapter, an app/ composition root) is
// free to pre-format a whole message and pass zero fields; `msg` and
// `fields` are independent, not alternatives.

#ifndef HAC_PORTS_PORT_LOG_H
#define HAC_PORTS_PORT_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN,
    LOG_LEVEL_INFO,
    LOG_LEVEL_DEBUG,
} log_level_t;

typedef enum {
    LOG_FIELD_I64 = 0,
    LOG_FIELD_U64,
    LOG_FIELD_F32,
    LOG_FIELD_BOOL,
    LOG_FIELD_STR,
} log_field_kind_t;

// One named value. `key` and, for LOG_FIELD_STR, `v.str` are borrowed: the
// adapter must not retain them past the write() call.
typedef struct {
    const char *key;
    log_field_kind_t kind;
    union {
        int64_t i64;
        uint64_t u64;
        float f32;
        bool b;
        const char *str;
    } v;
} log_field_t;

// Long enough for every call site this phase writes -- action + reason +
// partials + two booleans + a deadline is six. A call with more fields
// truncates at the adapter; the port itself imposes no allocation.
#define LOG_FIELDS_MAX 6

typedef struct port_log_s port_log_t;

struct port_log_s {
    // `tag`, `msg` and `fields` are borrowed for the duration of the call.
    // `msg` may be NULL when the fields alone carry the meaning; `fields` may
    // be NULL when `field_count` is 0.
    void (*write)(const port_log_t *self, log_level_t level, const char *tag, const char *msg,
                  const log_field_t *fields, size_t field_count);
    void *impl;
};

// NULL-safe: a NULL `log` (or one with no write() yet wired up) makes every
// call a no-op, so a module can take a port_log_t* before every caller has
// one to give it.
static inline void port_log_write(const port_log_t *log, log_level_t level, const char *tag,
                                  const char *msg, const log_field_t *fields, size_t field_count)
{
    if (log == NULL || log->write == NULL) {
        return;
    }
    log->write(log, level, tag, msg, fields, field_count);
}

// Field constructors, so a call site reads as a list of named values instead
// of union literals.

static inline log_field_t log_i64(const char *key, int64_t value)
{
    log_field_t f;
    f.key = key;
    f.kind = LOG_FIELD_I64;
    f.v.i64 = value;
    return f;
}

static inline log_field_t log_u64(const char *key, uint64_t value)
{
    log_field_t f;
    f.key = key;
    f.kind = LOG_FIELD_U64;
    f.v.u64 = value;
    return f;
}

static inline log_field_t log_f32(const char *key, float value)
{
    log_field_t f;
    f.key = key;
    f.kind = LOG_FIELD_F32;
    f.v.f32 = value;
    return f;
}

static inline log_field_t log_bool(const char *key, bool value)
{
    log_field_t f;
    f.key = key;
    f.kind = LOG_FIELD_BOOL;
    f.v.b = value;
    return f;
}

static inline log_field_t log_str(const char *key, const char *value)
{
    log_field_t f;
    f.key = key;
    f.kind = LOG_FIELD_STR;
    f.v.str = value;
    return f;
}

#ifdef __cplusplus
}
#endif

#endif  // HAC_PORTS_PORT_LOG_H
