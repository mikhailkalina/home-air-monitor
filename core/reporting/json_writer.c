// SPDX-License-Identifier: Apache-2.0

#include "reporting/json_writer.h"

#include "text/text_append.h"

// Below this a byte is a control character, which a JSON string may not hold.
#define JSON_FIRST_PRINTABLE 0x20u
#define JSON_HEX_DIGITS 4u

void json_begin(json_writer_t *w, char *buf, size_t cap)
{
    *w = (json_writer_t){.buf = buf, .cap = cap};
    if (cap == 0u) {
        w->misused = true;  // nowhere to put even the terminator
        return;
    }
    buf[0] = '\0';
}

static void put_char(json_writer_t *w, char c)
{
    w->len = text_append_char(w->buf, w->cap, w->len, c);
}

static void put_raw(json_writer_t *w, const char *s)
{
    w->len = text_append_str(w->buf, w->cap, w->len, s);
}

// What goes before any value: nothing after a key; a comma after an earlier
// item of an array. A value straight inside an object has no key, and a
// second value at the top makes two documents: both misuse.
static void before_value(json_writer_t *w)
{
    if (w->after_key) {
        w->after_key = false;
        return;
    }
    if (w->depth == 0u) {
        if (w->has_items[0]) {
            w->misused = true;
        }
        return;
    }
    if (w->in_object[w->depth - 1u]) {
        w->misused = true;
        return;
    }
    if (w->has_items[w->depth]) {
        put_char(w, ',');
    }
}

static void after_value(json_writer_t *w)
{
    w->has_items[w->depth] = true;
}

static void put_hex4(json_writer_t *w, unsigned value)
{
    static const char HEX[] = "0123456789abcdef";
    for (unsigned shift = JSON_HEX_DIGITS * 4u; shift > 0u; shift -= 4u) {
        put_char(w, HEX[(value >> (shift - 4u)) & 0xFu]);
    }
}

static void put_string(json_writer_t *w, const char *s)
{
    put_char(w, '"');
    for (; *s != '\0'; ++s) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            put_char(w, '\\');
            put_char(w, (char)c);
        } else if (c == '\n') {
            put_raw(w, "\\n");
        } else if (c == '\r') {
            put_raw(w, "\\r");
        } else if (c == '\t') {
            put_raw(w, "\\t");
        } else if (c < JSON_FIRST_PRINTABLE) {
            put_raw(w, "\\u");
            put_hex4(w, c);
        } else {
            put_char(w, (char)c);
        }
    }
    put_char(w, '"');
}

static void open_container(json_writer_t *w, bool object, char c)
{
    before_value(w);
    if (w->depth >= JSON_MAX_DEPTH) {
        w->misused = true;
        return;
    }
    put_char(w, c);
    w->in_object[w->depth] = object;
    w->depth++;
    w->has_items[w->depth] = false;
}

static void close_container(json_writer_t *w, bool object, char c)
{
    if (w->depth == 0u || w->in_object[w->depth - 1u] != object || w->after_key) {
        w->misused = true;
        return;
    }
    put_char(w, c);
    w->depth--;
    after_value(w);
}

void json_object_open(json_writer_t *w)
{
    open_container(w, true, '{');
}

void json_object_close(json_writer_t *w)
{
    close_container(w, true, '}');
}

void json_array_open(json_writer_t *w)
{
    open_container(w, false, '[');
}

void json_array_close(json_writer_t *w)
{
    close_container(w, false, ']');
}

void json_key(json_writer_t *w, const char *key)
{
    if (w->depth == 0u || !w->in_object[w->depth - 1u] || w->after_key) {
        w->misused = true;
        return;
    }
    if (w->has_items[w->depth]) {
        put_char(w, ',');
    }
    put_string(w, key);
    put_char(w, ':');
    w->has_items[w->depth] = true;
    w->after_key = true;
}

void json_string(json_writer_t *w, const char *s)
{
    before_value(w);
    put_string(w, s);
    after_value(w);
}

void json_fixed(json_writer_t *w, double value, unsigned decimals)
{
    before_value(w);
    if (text_fixed_representable(value, decimals)) {
        w->len = text_append_fixed(w->buf, w->cap, w->len, value, decimals);
    } else {
        put_raw(w, "null");
    }
    after_value(w);
}

void json_i64(json_writer_t *w, int64_t value)
{
    before_value(w);
    w->len = text_append_i64(w->buf, w->cap, w->len, value);
    after_value(w);
}

void json_u64(json_writer_t *w, uint64_t value)
{
    before_value(w);
    w->len = text_append_u64(w->buf, w->cap, w->len, value, 1u);
    after_value(w);
}

void json_bool(json_writer_t *w, bool value)
{
    before_value(w);
    put_raw(w, value ? "true" : "false");
    after_value(w);
}

void json_null(json_writer_t *w)
{
    before_value(w);
    put_raw(w, "null");
    after_value(w);
}

void json_member_string(json_writer_t *w, const char *key, const char *s)
{
    json_key(w, key);
    json_string(w, s);
}

void json_member_fixed(json_writer_t *w, const char *key, double value, unsigned decimals)
{
    json_key(w, key);
    json_fixed(w, value, decimals);
}

void json_member_i64(json_writer_t *w, const char *key, int64_t value)
{
    json_key(w, key);
    json_i64(w, value);
}

void json_member_u64(json_writer_t *w, const char *key, uint64_t value)
{
    json_key(w, key);
    json_u64(w, value);
}

void json_member_null(json_writer_t *w, const char *key)
{
    json_key(w, key);
    json_null(w);
}

void json_fail(json_writer_t *w)
{
    w->misused = true;
}

size_t json_end(const json_writer_t *w)
{
    const bool complete = w->depth == 0u && !w->after_key && w->has_items[0];
    if (w->misused || !complete || text_overflowed(w->len, w->cap)) {
        return 0u;
    }
    return w->len;
}
