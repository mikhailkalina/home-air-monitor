#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Static repository checks that do not require a compiler:
#   1. every first-party source file carries an SPDX identifier
#   2. core/ contains no platform-specific includes
#   3. no pin or I2C-address literal is hardcoded outside a platform's own
#      board_config.h
#
# Run locally with: ./scripts/check_sources.sh
# The pin-literal check (section 3) has its own fixture-driven test:
#   ./scripts/tests/test_pin_literal_check.sh

set -uo pipefail

red()  { printf '\033[31m%s\033[0m\n' "$*"; }
green(){ printf '\033[32m%s\033[0m\n' "$*"; }

# Paths holding vendored third-party code. Their original license headers must
# stay untouched, so they are exempt from every check below, including the
# pin-literal one: drivers/*/vendor/ will arrive in phase 3 full of raw
# register addresses that are not this project's pins to name.
EXCLUDE=(
    -not -path './*/third_party/*'
    -not -path './third_party/*'
    -not -path '*/vendor/*'
    -not -path './build*/*'
    -not -name '*.generated.*'
)

# Populates the global DIRS array with the source directories that exist
# under $1 (default: the current directory). Factored out from a plain
# top-level loop so scripts/tests/test_pin_literal_check.sh can point it at a
# fixture tree instead of the real repository and exercise the exact same
# directory-selection logic the real run uses.
hac_populate_dirs() {
    local root="${1:-.}"
    DIRS=()
    local d
    for d in ports core drivers platform apps tests; do
        [ -d "$root/$d" ] && DIRS+=("$root/$d")
    done
}

# Prints NUL-delimited paths of every first-party source file under the
# current DIRS, applying the same vendored/generated-file exclusions the
# SPDX check has always used. Shared by sections 1 and 3 (and by the
# pin-literal check's own test) so the two checks can never silently
# disagree about which files they scan.
hac_find_source_files() {
    [ ${#DIRS[@]} -eq 0 ] && return 0
    find "${DIRS[@]}" \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) \
         "${EXCLUDE[@]}" -print0
}

# Reads a NUL-delimited file list from stdin (as hac_find_source_files
# produces) and prints one "<file>: <line>" per pin or I2C-address literal
# found outside a platform's own board_config.h. A platform's board_config.h
# is where such a literal belongs, whichever platform it is -- adding a
# second platform is a supported scenario (docs/architecture.md section 11),
# so the exemption is by filename pattern, not by one fixed path.
#
# Reusing an existing BOARD_*_GPIO / BOARD_*_I2C_ADDRESS name elsewhere is the
# intended pattern and is never flagged: none of the patterns below can match
# an identifier, only a literal, because a C identifier cannot start with a
# digit.
#
# scripts/tests/fixtures/pin_literals/ carries one fixture per rule below,
# both a violation and (where a false positive is a real risk) the
# intended-reuse case that must stay clean -- see
# scripts/tests/test_pin_literal_check.sh.
pin_literal_violations() {
    local pattern
    pattern='GPIO_NUM_[0-9]+'
    pattern+='|#[[:space:]]*define[[:space:]]+[A-Z0-9_]*_(GPIO|I2C_ADDRESS)\b'
    # A bare numeric literal as the first argument of a gpio_*() call. Every
    # ESP-IDF gpio_* function takes the pin number first, so this needs no
    # per-function list; a legitimate BOARD_*_GPIO name can never start with
    # a digit, so this cannot mistake a named constant for a literal.
    pattern+='|\bgpio_[A-Za-z_]*\(\s*(0x[0-9A-Fa-f]+|[0-9]+)\b'
    # A bare numeric literal in the one argument position of the legacy I2C
    # driver (driver/i2c.h) that is genuinely a device address: the second
    # parameter of exactly these three functions. Deliberately NOT every
    # i2c_master_*() call -- i2c_master_write_byte()'s second argument is an
    # ordinary data byte written to the bus, not an address, and flagging it
    # would be exactly the kind of false positive that gets a check ignored.
    pattern+='|\bi2c_master_(write_to_device|read_from_device|write_read_device)'
    pattern+='\s*\(\s*[^,()]+,\s*(0x[0-9A-Fa-f]+|[0-9]+)\b'
    # The new i2c_master driver's own address field
    # (i2c_device_config_t::device_address, esp_driver_i2c/include/driver/
    # i2c_master.h), assigned a literal rather than a BOARD_*_I2C_ADDRESS
    # name.
    pattern+='|\.device_address\s*=\s*(0x[0-9A-Fa-f]+|[0-9]+)\b'

    local f hit line
    while IFS= read -r -d '' f; do
        if [[ "$f" =~ (^|/)platform/[^/]+/board_config\.h$ ]]; then
            continue
        fi
        hit=$(grep -En "$pattern" "$f") || continue
        while IFS= read -r line; do
            printf '%s: %s\n' "$f" "$line"
        done <<< "$hit"
    done
}

run_checks() {
    local STATUS=0

    # -----------------------------------------------------------------------
    # 1. SPDX headers
    # -----------------------------------------------------------------------
    if [ ${#DIRS[@]} -eq 0 ]; then
        echo "No source directories yet, skipping the SPDX check."
    else
        local missing=()
        local f
        while IFS= read -r -d '' f; do
            head -n 5 "$f" | grep -q 'SPDX-License-Identifier' || missing+=("$f")
        done < <(hac_find_source_files)

        if [ ${#missing[@]} -gt 0 ]; then
            red "Missing 'SPDX-License-Identifier' in the first 5 lines:"
            printf '  %s\n' "${missing[@]}"
            echo
            echo "Add this as the first line of each file, before the include guard:"
            echo "  // SPDX-License-Identifier: Apache-2.0"
            STATUS=1
        else
            green "SPDX headers: OK"
        fi
    fi

    # -----------------------------------------------------------------------
    # 2. Layering: the core must not know about any platform
    # -----------------------------------------------------------------------
    if [ -d core ]; then
        local leaks
        if leaks=$(grep -rEn '#include[[:space:]]+[<"](esp_|freertos/|driver/|SDL|stdio\.h|time\.h|stdlib\.h)' core/); then
            red "Platform-specific includes found in core/:"
            echo "$leaks" | sed 's/^/  /'
            echo
            echo "The core may only include ports/*.h and: stdint, stdbool, string, math."
            echo "Move the dependency behind a port in ports/ and implement it under platform/."
            STATUS=1
        else
            green "Core layering: OK"
        fi
    fi

    # -----------------------------------------------------------------------
    # 3. Pins and I2C addresses live in board_config.h and nowhere else
    # -----------------------------------------------------------------------
    if [ -d platform ]; then
        local pin_leaks=()
        local line
        while IFS= read -r line; do
            pin_leaks+=("$line")
        done < <(hac_find_source_files | pin_literal_violations)

        if [ ${#pin_leaks[@]} -gt 0 ]; then
            red "Pin or I2C-address literal found outside a platform's board_config.h:"
            printf '  %s\n' "${pin_leaks[@]}"
            echo
            echo "Add it to the platform's board_config.h as a named BOARD_*_GPIO (or"
            echo "BOARD_*_I2C_ADDRESS) constant and reference that name here instead --"
            echo "see CLAUDE.md: nothing else may hardcode a pin."
            STATUS=1
        else
            green "Pin literals: OK"
        fi
    fi

    return $STATUS
}

# Guards direct execution vs. being sourced: scripts/tests/test_pin_literal_check.sh
# sources this file to reuse hac_populate_dirs / hac_find_source_files /
# pin_literal_violations against its own fixture tree, without running the
# real checks against the repository or exiting the test's own shell.
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    cd "$(dirname "$0")/.."
    hac_populate_dirs "."
    run_checks
    exit $?
fi
