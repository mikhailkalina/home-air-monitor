// SPDX-License-Identifier: Apache-2.0
//
// core/reporting/json_writer.c: the text it writes, byte for byte; that a
// message which does not fit, or is put together wrongly, is refused whole;
// and the escaping RFC 8259 asks for. tests/unit/mqtt_messages_check.py
// parses what the reporting modules build on it with a real JSON parser.

#include "reporting/json_writer.h"

#include <math.h>
#include <string.h>

#include "hac_test.h"

static void members_and_nesting_get_their_commas_and_colons(void)
{
    char buf[128];
    json_writer_t w;
    json_begin(&w, buf, sizeof(buf));
    json_object_open(&w);
    json_member_string(&w, "a", "x");
    json_member_i64(&w, "b", -61);
    json_key(&w, "c");
    json_array_open(&w);
    json_u64(&w, 1u);
    json_bool(&w, true);
    json_null(&w);
    json_object_open(&w);
    json_object_close(&w);
    json_array_close(&w);
    json_member_fixed(&w, "d", 21.5, 2u);
    json_member_null(&w, "e");
    json_key(&w, "f");
    json_bool(&w, false);
    json_object_close(&w);
    const char *expected = "{\"a\":\"x\",\"b\":-61,\"c\":[1,true,null,{}],\"d\":21.50,"
                           "\"e\":null,\"f\":false}";
    HAC_CHECK_EQ_U64(json_end(&w), strlen(expected));
    HAC_CHECK(strcmp(buf, expected) == 0);
}

static void empty_containers_and_a_bare_value_are_documents(void)
{
    char buf[16];
    json_writer_t w;
    json_begin(&w, buf, sizeof(buf));
    json_object_open(&w);
    json_object_close(&w);
    HAC_CHECK_EQ_U64(json_end(&w), 2u);
    HAC_CHECK(strcmp(buf, "{}") == 0);

    json_begin(&w, buf, sizeof(buf));
    json_array_open(&w);
    json_array_close(&w);
    HAC_CHECK_EQ_U64(json_end(&w), 2u);
    HAC_CHECK(strcmp(buf, "[]") == 0);

    json_begin(&w, buf, sizeof(buf));
    json_string(&w, "");
    HAC_CHECK_EQ_U64(json_end(&w), 2u);
    HAC_CHECK(strcmp(buf, "\"\"") == 0);

    // Nothing written is not a document.
    json_begin(&w, buf, sizeof(buf));
    HAC_CHECK_EQ_U64(json_end(&w), 0u);
}

static void strings_are_escaped_as_rfc_8259_asks(void)
{
    char buf[128];
    json_writer_t w;
    json_begin(&w, buf, sizeof(buf));
    json_string(&w, "q\" b\\ n\n r\r t\t \x01\x1f del\x7f e\xC3\xA9 /");
    const char *expected = "\"q\\\" b\\\\ n\\n r\\r t\\t \\u0001\\u001f del\x7f e\xC3\xA9 /\"";
    HAC_CHECK_EQ_U64(json_end(&w), strlen(expected));
    HAC_CHECK(strcmp(buf, expected) == 0);

    // Keys too.
    json_begin(&w, buf, sizeof(buf));
    json_object_open(&w);
    json_member_u64(&w, "k\"", 1u);
    json_object_close(&w);
    HAC_CHECK(json_end(&w) != 0u);
    HAC_CHECK(strcmp(buf, "{\"k\\\"\":1}") == 0);
}

static void numbers_that_json_cannot_hold_are_null(void)
{
    char buf[64];
    json_writer_t w;
    json_begin(&w, buf, sizeof(buf));
    json_array_open(&w);
    json_fixed(&w, NAN, 1u);
    json_fixed(&w, INFINITY, 1u);
    json_fixed(&w, -INFINITY, 0u);
    json_fixed(&w, 1e20, 0u);
    json_fixed(&w, -0.0, 2u);
    json_fixed(&w, -5.25, 2u);
    json_array_close(&w);
    HAC_CHECK(json_end(&w) != 0u);
    HAC_CHECK(strcmp(buf, "[null,null,null,null,0.00,-5.25]") == 0);
}

// The exact length fits with its terminator; one byte less is refused whole.
static void a_buffer_exactly_large_enough_and_one_byte_short(void)
{
    const char *expected = "{\"rssi\":-61,\"s\":\"ok\"}";
    const size_t n = strlen(expected);
    char buf[64];
    json_writer_t w;

    json_begin(&w, buf, n + 1u);
    json_object_open(&w);
    json_member_i64(&w, "rssi", -61);
    json_member_string(&w, "s", "ok");
    json_object_close(&w);
    HAC_CHECK_EQ_U64(json_end(&w), n);
    HAC_CHECK(strcmp(buf, expected) == 0);

    memset(buf, 'X', sizeof(buf));
    json_begin(&w, buf, n);
    json_object_open(&w);
    json_member_i64(&w, "rssi", -61);
    json_member_string(&w, "s", "ok");
    json_object_close(&w);
    HAC_CHECK_EQ_U64(json_end(&w), 0u);
    HAC_CHECK(memchr(buf, '\0', n) != NULL);  // still terminated, never past n
    HAC_CHECK(buf[n] == 'X');

    // No room even for the terminator.
    json_begin(&w, buf, 0u);
    json_null(&w);
    HAC_CHECK_EQ_U64(json_end(&w), 0u);
}

static void misuse_is_refused_not_written(void)
{
    char buf[64];
    json_writer_t w;

    // A value in an object without a key.
    json_begin(&w, buf, sizeof(buf));
    json_object_open(&w);
    json_u64(&w, 1u);
    json_object_close(&w);
    HAC_CHECK_EQ_U64(json_end(&w), 0u);

    // A key in an array, and a key at the top.
    json_begin(&w, buf, sizeof(buf));
    json_array_open(&w);
    json_key(&w, "k");
    json_array_close(&w);
    HAC_CHECK_EQ_U64(json_end(&w), 0u);
    json_begin(&w, buf, sizeof(buf));
    json_key(&w, "k");
    HAC_CHECK_EQ_U64(json_end(&w), 0u);

    // A key without its value, and two keys in a row.
    json_begin(&w, buf, sizeof(buf));
    json_object_open(&w);
    json_key(&w, "k");
    json_object_close(&w);
    HAC_CHECK_EQ_U64(json_end(&w), 0u);
    json_begin(&w, buf, sizeof(buf));
    json_object_open(&w);
    json_key(&w, "k");
    json_key(&w, "l");
    json_null(&w);
    json_object_close(&w);
    HAC_CHECK_EQ_U64(json_end(&w), 0u);

    // A close that does not match, or with nothing open; one left open.
    json_begin(&w, buf, sizeof(buf));
    json_object_open(&w);
    json_array_close(&w);
    HAC_CHECK_EQ_U64(json_end(&w), 0u);
    json_begin(&w, buf, sizeof(buf));
    json_object_close(&w);
    HAC_CHECK_EQ_U64(json_end(&w), 0u);
    json_begin(&w, buf, sizeof(buf));
    json_array_open(&w);
    HAC_CHECK_EQ_U64(json_end(&w), 0u);

    // Two documents.
    json_begin(&w, buf, sizeof(buf));
    json_null(&w);
    json_null(&w);
    HAC_CHECK_EQ_U64(json_end(&w), 0u);

    // The caller giving up.
    json_begin(&w, buf, sizeof(buf));
    json_null(&w);
    json_fail(&w);
    HAC_CHECK_EQ_U64(json_end(&w), 0u);
}

static void nesting_stops_at_the_depth_limit(void)
{
    char buf[64];
    json_writer_t w;
    json_begin(&w, buf, sizeof(buf));
    for (unsigned i = 0u; i < JSON_MAX_DEPTH; ++i) {
        json_array_open(&w);
    }
    for (unsigned i = 0u; i < JSON_MAX_DEPTH; ++i) {
        json_array_close(&w);
    }
    HAC_CHECK_EQ_U64(json_end(&w), 2u * JSON_MAX_DEPTH);

    json_begin(&w, buf, sizeof(buf));
    for (unsigned i = 0u; i <= JSON_MAX_DEPTH; ++i) {
        json_array_open(&w);
    }
    for (unsigned i = 0u; i <= JSON_MAX_DEPTH; ++i) {
        json_array_close(&w);
    }
    HAC_CHECK_EQ_U64(json_end(&w), 0u);
}

int main(void)
{
    HAC_RUN(members_and_nesting_get_their_commas_and_colons);
    HAC_RUN(empty_containers_and_a_bare_value_are_documents);
    HAC_RUN(strings_are_escaped_as_rfc_8259_asks);
    HAC_RUN(numbers_that_json_cannot_hold_are_null);
    HAC_RUN(a_buffer_exactly_large_enough_and_one_byte_short);
    HAC_RUN(misuse_is_refused_not_written);
    HAC_RUN(nesting_stops_at_the_depth_limit);
    return hac_test_summary();
}
