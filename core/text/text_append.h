// SPDX-License-Identifier: Apache-2.0
//
// Text building without stdio (core/ has no snprintf), shared by the
// recordings (core/recording/) and the MQTT messages (core/reporting/).
// Every function appends to `dst` at `len`, never writes past `cap`, keeps
// `dst` terminated, and returns the new length; one that ran out of room
// returns `cap`, which is how a caller tells that the text did not fit --
// neither a recording nor a message is ever written half.
//
// Moved here from core/recording/record_text.h in phase 4a, when the MQTT
// payload came to need the same fixed-point numbers
// (docs/adr/0029-mqtt-topics-payload-and-discovery.md).

#ifndef HAC_CORE_TEXT_TEXT_APPEND_H
#define HAC_CORE_TEXT_TEXT_APPEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// `len` == `cap` marks text that did not fit, and every function passes it on.
size_t text_append_str(char *dst, size_t cap, size_t len, const char *src);
size_t text_append_char(char *dst, size_t cap, size_t len, char c);

// At least `width` digits, zero-padded on the left.
size_t text_append_u64(char *dst, size_t cap, size_t len, uint64_t value, unsigned width);

// A signed integer, "-" and its digits; INT64_MIN included.
size_t text_append_i64(char *dst, size_t cap, size_t len, int64_t value);

// True when text_append_fixed() can write `value` with `decimals`: finite,
// and small enough for the arithmetic. A caller that must say why a number
// is missing asks this first, so that "no room" and "no number" are not the
// same answer.
bool text_fixed_representable(double value, unsigned decimals);

// `value` with exactly `decimals` digits after the point, rounded half away
// from zero ("-0.50", "1006.123", "620"). A value text_fixed_representable()
// refuses does not fit (returns `cap`): a recording or a payload holds a
// number or nothing, never a placeholder that reads like one.
size_t text_append_fixed(char *dst, size_t cap, size_t len, double value, unsigned decimals);

// True when the text built so far did not fit.
static inline bool text_overflowed(size_t len, size_t cap)
{
    return len >= cap;
}

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_TEXT_TEXT_APPEND_H
