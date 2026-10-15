#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Proves scripts/check_sources.sh's driver-include check (section 4) fires on
# exactly the violations in scripts/tests/fixtures/driver_includes/ and stays
# silent on the near-misses, the vendored file and the platform file there.
# A check that has never been seen to fail is not known to work.
#
# Run locally with: ./scripts/tests/test_driver_include_check.sh

set -uo pipefail
cd "$(dirname "$0")/../.."

red()  { printf '\033[31m%s\033[0m\n' "$*"; }
green(){ printf '\033[32m%s\033[0m\n' "$*"; }

# Sourcing rather than executing: the guard at the bottom of check_sources.sh
# skips the real run and just loads its functions into this shell.
# shellcheck source=../check_sources.sh
source scripts/check_sources.sh

FIXTURE="scripts/tests/fixtures/driver_includes"
hac_populate_dirs "$FIXTURE"

violations=$(hac_find_source_files | driver_include_violations)
violation_count=$(printf '%s\n' "$violations" | grep -c . || true)

STATUS=0

# One entry per include in drivers/somechip/chip_violations.c -- one per
# forbidden prefix, the last written with a space after the '#'.
EXPECTED=(
    'chip_violations\.c:.*#include <esp_log\.h>'
    'chip_violations\.c:.*#include "freertos/FreeRTOS\.h"'
    'chip_violations\.c:.*#include "driver/i2c_master\.h"'
    'chip_violations\.c:.*#include <SDL2/SDL\.h>'
    'chip_violations\.c:.*# include <stdio\.h>'
)

for pattern in "${EXPECTED[@]}"; do
    if printf '%s\n' "$violations" | grep -Eq "$pattern"; then
        green "flagged as expected: $pattern"
    else
        red "NOT flagged, but should be: $pattern"
        STATUS=1
    fi
done

# None of these must appear in the violation list at all:
#   - chip_allowed.c: the includes a driver may use, and a near-miss of each
#     forbidden prefix (driver_utils.h, sdl_free_name.h, stdio_like.h,
#     espresso.h, my_freertos_shim.h)
#   - vendor/vendor_chip.c: vendored code, excluded before the check runs
#   - platform/someboard/adp_chip.c: an adapter, where ESP-IDF belongs
FORBIDDEN=(
    "chip_allowed.c"
    "vendor/vendor_chip.c"
    "platform/someboard/adp_chip.c"
)

for needle in "${FORBIDDEN[@]}"; do
    if printf '%s\n' "$violations" | grep -qF "$needle"; then
        red "flagged, but should stay clean: $needle"
        STATUS=1
    else
        green "clean as expected: $needle"
    fi
done

# Catches the check firing on MORE than intended, not just less.
if [ "$violation_count" -ne "${#EXPECTED[@]}" ]; then
    red "expected exactly ${#EXPECTED[@]} violation lines, got $violation_count:"
    printf '%s\n' "$violations" | sed 's/^/  /'
    STATUS=1
fi

if [ $STATUS -eq 0 ]; then
    green "driver-include check fixture test: OK"
else
    red "driver-include check fixture test: FAILED"
fi
exit $STATUS
