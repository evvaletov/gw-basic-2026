#!/bin/bash
# Run all .bas test programs and report results.
# If .expected files exist, also compare output against them.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
GWBASIC="${PROJECT_DIR}/build/gwbasic"
EXPECTED_DIR="${SCRIPT_DIR}/expected"

if [ ! -x "$GWBASIC" ]; then
    echo "ERROR: gwbasic not found at $GWBASIC (run cmake/make first)"
    exit 1
fi

pass=0
fail=0
compat_pass=0
compat_fail=0

for bas in "$SCRIPT_DIR"/programs/*.bas; do
    name="$(basename "$bas")"
    stem="${name%.bas}"
    # chain/common targets are not standalone
    [ "$name" = "chain_target.bas" ] && continue
    [ "$name" = "common_target.bas" ] && continue

    actual=$(mktemp)
    if timeout 5 "$GWBASIC" "$bas" > "$actual" 2>&1; then
        printf "  PASS  %s" "$name"
        pass=$((pass + 1))

        # Compare against .expected if available
        expected="$EXPECTED_DIR/${stem}.expected"
        if [ -f "$expected" ]; then
            normalized=$(mktemp)
            sed 's/\r//g; s/[[:space:]]*$//' "$actual" | sed '/^$/d' > "$normalized"
            if diff -q "$expected" "$normalized" >/dev/null 2>&1; then
                printf "  [compat: ok]"
                compat_pass=$((compat_pass + 1))
            else
                printf "  [compat: MISMATCH]"
                compat_fail=$((compat_fail + 1))
            fi
            rm -f "$normalized"
        fi
        printf "\n"
    else
        printf "  FAIL  %s\n" "$name"
        fail=$((fail + 1))
    fi
    rm -f "$actual"
done

echo ""
echo "$((pass + fail)) tests: $pass passed, $fail failed"
if [ "$((compat_pass + compat_fail))" -gt 0 ]; then
    echo "Compat: $compat_pass matched, $compat_fail mismatched"
fi
[ "$fail" -eq 0 ] || exit 1
