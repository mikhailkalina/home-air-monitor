// SPDX-License-Identifier: Apache-2.0
//
// libFuzzer harness for replay_parse_buffer(): the parser must never crash,
// leak or read out of bounds on arbitrary bytes, whether or not it accepts
// them as a valid scenario file. Run for real with Clang (see
// tests/fuzz/CMakeLists.txt); ASan/UBSan on the host build cover the same
// function against the handful of fixed cases in tests/unit/test_replay_source.c.

#include <stddef.h>
#include <stdint.h>

#include "replay/replay_source.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    replay_timeline_t tl;
    if (replay_parse_buffer((const char *)data, size, &tl) == HAL_OK) {
        replay_timeline_dispose(&tl);
    }
    return 0;
}
