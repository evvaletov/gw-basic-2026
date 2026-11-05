#!/bin/bash
# Run all .bas test programs and report results.
# If .expected files exist, also compare output against them.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
GWBASIC="${PROJECT_DIR}/build/gwbasic"
EXPECTED_DIR="${SCRIPT_DIR}/expected"
source "$SCRIPT_DIR/harness_common.sh"

if [ ! -x "$GWBASIC" ]; then
    echo "ERROR: gwbasic not found at $GWBASIC (run cmake/make first)"
    exit 1
fi

WORK_DIR=$(mktemp -d) || exit 1
trap 'rm -rf "$WORK_DIR"' EXIT
cd "$PROJECT_DIR" || exit 1
shopt -s nullglob

pass=0
fail=0
skip=0
compat_pass=0
compat_fail=0

for bas in "$SCRIPT_DIR"/programs/*.bas; do
    name="$(basename "$bas")"
    stem="${name%.bas}"
    # chain/common targets are not standalone
    if [ "$name" = "chain_target.bas" ] || [ "$name" = "common_target.bas" ]; then
        printf "  SKIP  %s (helper program)\n" "$name"
        skip=$((skip + 1))
        continue
    fi

    actual="$WORK_DIR/$stem.out"
    limit=$(runtime_timeout "$name")
    timeout --kill-after=2 "$limit" "$GWBASIC" "$bas" </dev/null > "$actual" 2>&1
    status=$?
    if [ "$status" -ne 0 ]; then
        report_process_failure RUN "$name" "$status" "$limit" "$actual"
        fail=$((fail + 1))
        continue
    fi

    expected="$EXPECTED_DIR/${stem}.expected"
    if [ -f "$expected" ]; then
        compare_output "$expected" "$actual" "$WORK_DIR" > "$WORK_DIR/diff" 2>&1
        status=$?
        if [ "$status" -ne 0 ]; then
            if [ "$status" -eq 1 ]; then
                printf "  FAIL  %s [compat: MISMATCH]\n" "$name"
                compat_fail=$((compat_fail + 1))
            else
                printf "  COMPARE-FAIL  %s\n" "$name"
            fi
            cat "$WORK_DIR/diff"
            fail=$((fail + 1))
            continue
        fi
        compat_pass=$((compat_pass + 1))
        printf "  PASS  %s [compat: ok]\n" "$name"
    else
        printf "  PASS  %s [no expected]\n" "$name"
    fi
    pass=$((pass + 1))
done

echo ""
echo "$((pass + fail)) tests: $pass passed, $fail failed ($skip skipped)"
if [ "$((compat_pass + compat_fail))" -gt 0 ]; then
    echo "Compat: $compat_pass matched, $compat_fail mismatched"
fi
if [ "$((pass + fail))" -eq 0 ]; then
    echo "ERROR: no runnable interpreter tests found" >&2
    exit 1
fi
[ "$fail" -eq 0 ] || exit 1
