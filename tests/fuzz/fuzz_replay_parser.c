// SPDX-License-Identifier: Apache-2.0
//
// libFuzzer harness for replay_parse_buffer(): the parser must never crash,
// leak or read out of bounds on arbitrary bytes, whether or not it accepts
// them as a valid scenario file. Run for real with Clang (see
// tests/fuzz/CMakeLists.txt); ASan/UBSan on the host build cover the same
// function against the handful of fixed cases in tests/unit/test_replay_source.c.
//
// What it accepts is played back too, since phase 3f made playback depend on
// the file in more ways -- per-field statuses, accuracy, rows that interpolate
// field by field (docs/adr/0024): at its start, between its rows, past its
// loop point, with interpolation on and off, and its capabilities read from
// the header. tests/fuzz/corpus/ seeds the column forms the recorder writes.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "replay/replay_source.h"

// Playback points, in milliseconds since the scenario's start: the start, a
// few inside, and far past any loop point.
static const uint64_t ELAPSED_MS[] = {0u, 1u, 999u, 5000u, 60000u, 86400000u, UINT64_MAX / 2u};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    replay_timeline_t tl;
    if (replay_parse_buffer((const char *)data, size, &tl) != HAL_OK) {
        return 0;
    }
    (void)replay_timeline_caps(&tl);
    (void)replay_timeline_accuracy_caps(&tl);
    for (size_t i = 0u; i < sizeof(ELAPSED_MS) / sizeof(ELAPSED_MS[0]); ++i) {
        sensor_sample_t sample;
        replay_timeline_sample(&tl, ELAPSED_MS[i], true, 1u, 1u, &sample);
        replay_timeline_sample(&tl, ELAPSED_MS[i], false, 1u, 1u, &sample);
    }
    replay_timeline_dispose(&tl);
    return 0;
}
