#!/bin/bash
# Run all .bas test programs and report results.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
GWBASIC="${PROJECT_DIR}/build/gwbasic"

if [ ! -x "$GWBASIC" ]; then
    echo "ERROR: gwbasic not found at $GWBASIC (run cmake/make first)"
    exit 1
fi

pass=0
fail=0

for bas in "$SCRIPT_DIR"/programs/*.bas; do
    name="$(basename "$bas")"
    # chain_target.bas is not standalone
    [ "$name" = "chain_target.bas" ] && continue
    # graphics_stubs.bas expects real graphics now; skip if SCREEN causes issues
    if timeout 5 "$GWBASIC" "$bas" >/dev/null 2>&1; then
        printf "  PASS  %s\n" "$name"
        pass=$((pass + 1))
    else
        printf "  FAIL  %s\n" "$name"
        fail=$((fail + 1))
    fi
done

echo ""
echo "$((pass + fail)) tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
