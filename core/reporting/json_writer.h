// SPDX-License-Identifier: Apache-2.0
//
// A JSON text built into the caller's buffer: objects, arrays, strings,
// numbers, booleans and null, with the commas and colons where they belong,
// no whitespace, and nothing allocated. Built on core/text/text_append.h,
// with its convention: once something does not fit, everything after it is
// dropped, and json_end() says so -- a message is whole or not written.
//
// Misuse is caught the same way rather than written: a value in an object
// without its key, a key outside an object, a close that does not match its
// open, nesting deeper than JSON_MAX_DEPTH, a second top-level value. Any of
// them makes json_end() return 0, so a malformed message is never sent.
//
// Strings are escaped as RFC 8259 requires: the quote, the backslash and
// every control character below 0x20. Other bytes pass through, so text
// that is valid UTF-8 stays valid UTF-8; the caller owns that.

#ifndef HAC_CORE_REPORTING_JSON_WRITER_H
#define HAC_CORE_REPORTING_JSON_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Deep enough for a discovery message: the message, its device block, the
// device's identifiers.
#define JSON_MAX_DEPTH 4u

typedef struct {
    char *buf;
    size_t cap;
    size_t len;  // == cap once something did not fit
    unsigned depth;
    bool in_object[JSON_MAX_DEPTH];       // per open container: object or array
    bool has_items[JSON_MAX_DEPTH + 1u];  // per level, the top included
    bool after_key;
    bool misused;
} json_writer_t;

// Start an empty text in `buf` (terminated, when `cap` allows).
void json_begin(json_writer_t *w, char *buf, size_t cap);

void json_object_open(json_writer_t *w);
void json_object_close(json_writer_t *w);
void json_array_open(json_writer_t *w);
void json_array_close(json_writer_t *w);

// The key of the next value, inside an object.
void json_key(json_writer_t *w, const char *key);

void json_string(json_writer_t *w, const char *s);
// A number with exactly `decimals` digits after the point (text_append.h's
// rounding, never "-0"); null when text_fixed_representable() refuses it,
// since JSON has no NaN or infinity. A caller that must say why a number
// is missing checks first.
void json_fixed(json_writer_t *w, double value, unsigned decimals);
void json_i64(json_writer_t *w, int64_t value);
void json_u64(json_writer_t *w, uint64_t value);
void json_bool(json_writer_t *w, bool value);
void json_null(json_writer_t *w);

// A key and its value in one call.
void json_member_string(json_writer_t *w, const char *key, const char *s);
void json_member_fixed(json_writer_t *w, const char *key, double value, unsigned decimals);
void json_member_i64(json_writer_t *w, const char *key, int64_t value);
void json_member_u64(json_writer_t *w, const char *key, uint64_t value);
void json_member_null(json_writer_t *w, const char *key);

// The caller could not build what it meant to write: the message is
// refused, as if it had not fit.
void json_fail(json_writer_t *w);

// The text's length: one complete value, every container closed. 0 when it
// did not fit or was misused; the buffer then holds no usable message.
size_t json_end(const json_writer_t *w);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_REPORTING_JSON_WRITER_H
