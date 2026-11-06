#!/bin/bash
# Run all .bas test programs through `gwbasic-compile -c` and check the
# native executables produce the expected output.  Mirrors
# tests/run_tests.sh but exercises the AOT compiler path.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
COMPILE="${PROJECT_DIR}/build/gwbasic-compile"
EXPECTED_DIR="${SCRIPT_DIR}/expected"
source "$SCRIPT_DIR/harness_common.sh"
WORK_DIR=$(mktemp -d) || exit 1
trap 'rm -rf "$WORK_DIR"' EXIT

if [ ! -x "$COMPILE" ]; then
    echo "ERROR: gwbasic-compile not found at $COMPILE (run cmake/make first)" >&2
    exit 1
fi
cd "$PROJECT_DIR" || exit 1
shopt -s nullglob
if [ ! -f "$PROJECT_DIR/build/libgwrt.a" ]; then
    echo "ERROR: libgwrt.a not built yet (run cmake/make first)" >&2
    exit 1
fi

# Programs not meaningful for the AOT path:
#   - chain_target.bas / common_target.bas are not standalone (loaded via
#     CHAIN, never run directly)
#   - interactive / timing-dependent / hardware-output programs
SKIP=(
    chain_target.bas
    common_target.bas
    datetime.bas
    on_timer.bas
    timer_stop.bas
    color_test.bas
    sound_test.bas
    play_music.bas
    play_scale.bas
    speaker_out.bas
    text_adventure.bas
)
should_skip() {
    local n="$1"
    for s in "${SKIP[@]}"; do [ "$n" = "$s" ] && return 0; done
    return 1
}

pass=0
fail=0
skip=0
for bas in "$SCRIPT_DIR"/programs/*.bas; do
    name="$(basename "$bas")"
    stem="${name%.bas}"
    if should_skip "$name"; then
        printf "  SKIP  %s\n" "$name"
        skip=$((skip + 1))
        continue
    fi

    if ! cp "$bas" "$WORK_DIR/$name"; then
        printf "  COPY-FAIL  %s\n" "$name"
        fail=$((fail + 1))
        continue
    fi
    compile_log="$WORK_DIR/$stem.compile.log"
    ( cd "$WORK_DIR" && timeout --kill-after=2 "$GW_TEST_COMPILE_TIMEOUT" \
        "$COMPILE" -c --runtime "$PROJECT_DIR" "$name" ) </dev/null > "$compile_log" 2>&1
    status=$?
    if [ "$status" -ne 0 ]; then
        report_process_failure COMPILE "$name" "$status" "$GW_TEST_COMPILE_TIMEOUT" "$compile_log"
        fail=$((fail + 1))
        continue
    fi
    if [ ! -x "$WORK_DIR/$stem" ]; then
        printf "  NO-EXE  %s\n" "$name"
        fail=$((fail + 1))
        continue
    fi

    actual="$WORK_DIR/$stem.out"
    limit=$(runtime_timeout "$name")
    timeout --kill-after=2 "$limit" "$WORK_DIR/$stem" </dev/null > "$actual" 2>&1
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
                printf "  DIFF  %s\n" "$name"
            else
                printf "  COMPARE-FAIL  %s\n" "$name"
            fi
            cat "$WORK_DIR/diff"
            fail=$((fail + 1))
            continue
        fi
        printf "  PASS  %s\n" "$name"
    else
        # No golden file: just confirm it ran without crashing.
        printf "  PASS  %s  [no expected]\n" "$name"
    fi
    pass=$((pass + 1))
done

echo ""
echo "$((pass + fail)) compiled tests: $pass passed, $fail failed ($skip skipped)"
if [ "$((pass + fail))" -eq 0 ]; then
    echo "ERROR: no runnable compiler tests found" >&2
    exit 1
fi
[ "$fail" -eq 0 ] || exit 1
