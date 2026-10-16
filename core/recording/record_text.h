// SPDX-License-Identifier: Apache-2.0
//
// Text building for the recordings, without stdio (core/ has no snprintf).
// Every function appends to `dst` at `len`, never writes past `cap`, keeps
// `dst` terminated, and returns the new length; one that ran out of room
// returns `cap`, which is how a caller tells that the text did not fit --
// the recorder never writes half a row.

#ifndef HAC_CORE_RECORDING_RECORD_TEXT_H
#define HAC_CORE_RECORDING_RECORD_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// `len` == `cap` marks text that did not fit, and every function passes it on.
size_t record_text_str(char *dst, size_t cap, size_t len, const char *src);
size_t record_text_char(char *dst, size_t cap, size_t len, char c);

// At least `width` digits, zero-padded on the left.
size_t record_text_u64(char *dst, size_t cap, size_t len, uint64_t value, unsigned width);

// `value` with exactly `decimals` digits after the point, rounded half away
// from zero ("-0.50", "1006.123", "620"). A non-finite value, or one too
// large for the arithmetic, does not fit (returns `cap`): a recording holds
// a number or nothing, never a placeholder that reads like one.
size_t record_text_fixed(char *dst, size_t cap, size_t len, double value, unsigned decimals);

// True when the text built so far did not fit.
static inline bool record_text_overflowed(size_t len, size_t cap)
{
    return len >= cap;
}

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_RECORDING_RECORD_TEXT_H
