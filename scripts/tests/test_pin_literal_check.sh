#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Proves scripts/check_sources.sh's pin-literal check (section 3) fires on
# exactly the violations in scripts/tests/fixtures/pin_literals/ and stays
# silent on every intended-reuse and exempted case there. A check that has
# never been seen to fail is not known to work.
#
# Run locally with: ./scripts/tests/test_pin_literal_check.sh

set -uo pipefail
cd "$(dirname "$0")/../.."

red()  { printf '\033[31m%s\033[0m\n' "$*"; }
green(){ printf '\033[32m%s\033[0m\n' "$*"; }

# Sourcing rather than executing: the guard at the bottom of check_sources.sh
# skips the real run and just loads hac_populate_dirs(), hac_find_source_files()
# and pin_literal_violations() into this shell.
# shellcheck source=../check_sources.sh
source scripts/check_sources.sh

FIXTURE="scripts/tests/fixtures/pin_literals"
hac_populate_dirs "$FIXTURE"

violations=$(hac_find_source_files | pin_literal_violations)
violation_count=$(printf '%s\n' "$violations" | grep -c . || true)

STATUS=0

# One entry per distinct construct in
# platform/other_board/adp_violations.c -- each exercises exactly one rule in
# pin_literal_violations(). Regex special characters in the fixture source
# (parens, the dot in ".device_address") are escaped so grep -E matches them
# literally.
EXPECTED=(
    'adp_violations\.c:.*#define STRAY_NEW_GPIO 7'
    'adp_violations\.c:.*gpio_set_direction\(9, 1\)'
    'adp_violations\.c:.*GPIO_NUM_9'
    'adp_violations\.c:.*i2c_master_write_to_device\(0, 0x5A'
    'adp_violations\.c:.*\.device_address = 0x5A'
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
#   - adp_allowed.c: the intended-reuse case, and the specific near-miss
#     (i2c_master_write_byte, not one of the three device-address functions)
#   - other_board/board_config.h: a second platform's own board_config.h,
#     exempt by filename pattern (gap this test exists to close)
#   - vendor/reg_defs.h: never even reaches pin_literal_violations, because
#     hac_find_source_files applies the same vendor/ exclusion the SPDX
#     check uses
FORBIDDEN=(
    "adp_allowed.c"
    "other_board/board_config.h"
    "vendor/reg_defs.h"
)

for needle in "${FORBIDDEN[@]}"; do
    if printf '%s\n' "$violations" | grep -qF "$needle"; then
        red "flagged, but should stay clean: $needle"
        STATUS=1
    else
        green "clean as expected: $needle"
    fi
done

# Catches the check firing on MORE than intended, not just less: five
# distinct constructs in the fixture, five expected lines. Passing every
# check above but reporting a sixth line would still be a bug.
if [ "$violation_count" -ne "${#EXPECTED[@]}" ]; then
    red "expected exactly ${#EXPECTED[@]} violation lines, got $violation_count:"
    printf '%s\n' "$violations" | sed 's/^/  /'
    STATUS=1
fi

if [ $STATUS -eq 0 ]; then
    green "pin-literal check fixture test: OK"
else
    red "pin-literal check fixture test: FAILED"
fi
exit $STATUS
