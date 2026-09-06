// SPDX-License-Identifier: Apache-2.0
//
// The plumbing shared by every golden test: compare a rendered framebuffer
// against tests/golden/<name>.bin, and on a mismatch dump both images as PNG
// under the build directory for a human to look at.
//
// The assertion itself is a byte comparison. That is exact, needs no image
// library, and fails on a one-pixel change -- which is what a golden test is
// for. The PNGs are a diagnostic only and never decide pass or fail.

#ifndef HAC_TESTS_GOLDEN_CHECK_H
#define HAC_TESTS_GOLDEN_CHECK_H

#include <stdbool.h>

#include "ui/framebuffer.h"

// Compares fb->pixels against `golden_dir`/`name`.bin.
//
// With UPDATE_GOLDEN set in the environment, writes the reference instead and
// returns true -- the way to adopt an intentional rendering change:
//   UPDATE_GOLDEN=1 ./build/host-debug/tests/golden/test_golden_screen_home
bool golden_check_framebuffer(const framebuffer_t *fb, const char *golden_dir, const char *dump_dir,
                              const char *name);

#endif  // HAC_TESTS_GOLDEN_CHECK_H
