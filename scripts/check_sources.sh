#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Static repository checks that do not require a compiler:
#   1. every first-party source file carries an SPDX identifier
#   2. core/ contains no platform-specific includes
#
# Run locally with: ./scripts/check_sources.sh

set -uo pipefail
cd "$(dirname "$0")/.."

STATUS=0
red()  { printf '\033[31m%s\033[0m\n' "$*"; }
green(){ printf '\033[32m%s\033[0m\n' "$*"; }

# Paths holding vendored third-party code. Their original license headers must
# stay untouched, so they are exempt from the SPDX check.
EXCLUDE=(
    -not -path './*/third_party/*'
    -not -path './third_party/*'
    -not -path '*/vendor/*'
    -not -path './build*/*'
    -not -name '*.generated.*'
)

# Directories that may not exist yet in the early phases.
DIRS=()
for d in ports core drivers platform apps tests; do
    [ -d "$d" ] && DIRS+=("$d")
done

# ---------------------------------------------------------------------------
# 1. SPDX headers
# ---------------------------------------------------------------------------
if [ ${#DIRS[@]} -eq 0 ]; then
    echo "No source directories yet, skipping the SPDX check."
else
    missing=()
    while IFS= read -r -d '' f; do
        head -n 5 "$f" | grep -q 'SPDX-License-Identifier' || missing+=("$f")
    done < <(find "${DIRS[@]}" \
                  \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) \
                  "${EXCLUDE[@]}" -print0)

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

# ---------------------------------------------------------------------------
# 2. Layering: the core must not know about any platform
# ---------------------------------------------------------------------------
if [ -d core ]; then
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

exit $STATUS
