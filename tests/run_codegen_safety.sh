#!/bin/bash
# Codegen termination / bounded-output guard.
#
# Regression coverage for the 2026-06-13 runaway incident (issue 44563c0): a
# codegen path that emitted C without advancing the token pointer, which an
# enclosing statement loop re-invoked forever, growing the output ~30 MB/s
# until the disk filled.  The source fix checks per-statement token progress
# (emit_stmt_guarded) and caps output size. This test checks that the fixtures,
# including INSTR and string-expression stress inputs, finish within the time
# and output budgets below. It does not prove termination for all inputs.
#
# Every program must:
#   - finish code generation successfully within TIMEOUT seconds, and
#   - produce a .c file no larger than MAX_C_BYTES.
# The largest output currently in the suite is about 27 KB, leaving headroom
# below the 512 KB ceiling for expected code growth.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
COMPILE="${PROJECT_DIR}/build/gwbasic-compile"
TIMEOUT=20
MAX_C_BYTES=524288          # 512 KB per-program ceiling
HARD_CAP_MB=8               # --max-output-size backstop (compiler-side)

if [ ! -x "$COMPILE" ]; then
    echo "ERROR: gwbasic-compile not found at $COMPILE (build first)" >&2
    exit 1
fi

WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

pass=0
fail=0
biggest=0
biggest_name=""

for bas in "$SCRIPT_DIR"/programs/*.bas "$SCRIPT_DIR"/classic/*.bas; do
    [ -f "$bas" ] || continue
    name="$(basename "$bas")"
    stem="${name%.bas}"
    cp "$bas" "$WORK_DIR/$name"
    cfile="$WORK_DIR/${stem}.c"
    # Transpile-only (no -c): emit C to a file, no gcc, so this exercises
    # codegen in isolation and stays fast.
    timeout "$TIMEOUT" "$COMPILE" -o "$cfile" \
        --max-output-size "$HARD_CAP_MB" \
        --runtime "$PROJECT_DIR" "$WORK_DIR/$name" >/dev/null 2>&1
    rc=$?
    if [ "$rc" -eq 124 ]; then
        printf "  TIMEOUT  %s (codegen did not terminate in %ss)\n" "$name" "$TIMEOUT"
        fail=$((fail + 1))
        continue
    fi
    if [ "$rc" -ne 0 ]; then
        printf "  ERROR    %s (codegen exited %d)\n" "$name" "$rc"
        fail=$((fail + 1))
        continue
    fi
    if [ ! -s "$cfile" ]; then
        # Successful transpilation must leave a nonempty C translation unit.
        printf "  NO-C     %s (no C emitted, rc=%d)\n" "$name" "$rc"
        fail=$((fail + 1))
        continue
    fi
    bytes=$(wc -c < "$cfile")
    if [ "$bytes" -gt "$biggest" ]; then biggest=$bytes; biggest_name=$name; fi
    if [ "$bytes" -gt "$MAX_C_BYTES" ]; then
        printf "  TOO-BIG  %s (%d bytes > %d ceiling)\n" "$name" "$bytes" "$MAX_C_BYTES"
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
    rm -f "$cfile"
done

echo ""
echo "codegen safety: $pass ok, $fail failed (largest C: $biggest bytes in ${biggest_name:-none})"
[ "$fail" -eq 0 ] || exit 1
