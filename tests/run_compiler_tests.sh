#!/bin/bash
# Run all .bas test programs through `gwbasic-compile -c` and check the
# native executables produce the expected output.  Mirrors
# tests/run_tests.sh but exercises the AOT compiler path.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
COMPILE="${PROJECT_DIR}/build/gwbasic-compile"
EXPECTED_DIR="${SCRIPT_DIR}/expected"
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

if [ ! -x "$COMPILE" ]; then
    echo "ERROR: gwbasic-compile not found at $COMPILE (run cmake/make first)" >&2
    exit 1
fi
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

    cp "$bas" "$WORK_DIR/$name"
    pushd "$WORK_DIR" > /dev/null
    if ! "$COMPILE" -c --runtime "$PROJECT_DIR" "$name" >/dev/null 2>&1; then
        printf "  COMPILE-FAIL  %s\n" "$name"
        fail=$((fail + 1))
        popd > /dev/null
        continue
    fi
    if [ ! -x "$WORK_DIR/$stem" ]; then
        printf "  NO-EXE  %s\n" "$name"
        fail=$((fail + 1))
        popd > /dev/null
        continue
    fi
    popd > /dev/null

    # Run from project root so test programs that reference tests/programs/
    # (chain_test, common_test, run_file, misc_stmts) resolve relative paths.
    actual=$(mktemp)
    if ! ( cd "$PROJECT_DIR" && timeout 5 "$WORK_DIR/$stem" > "$actual" 2>&1 ); then
        printf "  RUN-FAIL  %s\n" "$name"
        fail=$((fail + 1))
        rm -f "$actual"
        continue
    fi

    expected="$EXPECTED_DIR/${stem}.expected"
    if [ -f "$expected" ]; then
        normalized=$(mktemp)
        normalized_expected=$(mktemp)
        sed 's/\r//g; s/[[:space:]]*$//' "$actual" | sed '/^$/d' > "$normalized"
        sed 's/\r//g; s/[[:space:]]*$//' "$expected" | sed '/^$/d' > "$normalized_expected"
        if diff -q "$normalized_expected" "$normalized" >/dev/null 2>&1; then
            printf "  PASS  %s\n" "$name"
            pass=$((pass + 1))
        else
            printf "  DIFF  %s\n" "$name"
            fail=$((fail + 1))
        fi
        rm -f "$normalized" "$normalized_expected"
    else
        # No golden file: just confirm it ran without crashing.
        printf "  PASS  %s  [no expected]\n" "$name"
        pass=$((pass + 1))
    fi
    rm -f "$actual"
done

echo ""
echo "$((pass + fail)) compiled tests: $pass passed, $fail failed ($skip skipped)"
[ "$fail" -eq 0 ] || exit 1
