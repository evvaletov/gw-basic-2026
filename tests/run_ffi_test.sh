#!/bin/bash
# Exercise the Level 2 cross-language path: compile a BASIC program that calls
# C functions via '$EXTERN pragmas, link it against a companion C object, run
# it, and compare against expected output.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
COMPILE="${PROJECT_DIR}/build/gwbasic-compile"
FFI_DIR="${SCRIPT_DIR}/ffi"
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

cp "$FFI_DIR/extern_demo.bas" "$FFI_DIR/extern_lib.c" "$WORK_DIR/"
cd "$WORK_DIR" || exit 1

# BASIC -> object (entry point stays main; the C lib provides only helpers)
if ! "$COMPILE" extern_demo.bas --emit-obj --runtime "$PROJECT_DIR" >/dev/null 2>&1; then
    echo "FAIL: gwbasic-compile --emit-obj failed" >&2
    exit 1
fi
gcc -c extern_lib.c -o extern_lib.o || { echo "FAIL: C lib compile" >&2; exit 1; }

LINK="gcc extern_demo.o extern_lib.o -o extern_demo -L$PROJECT_DIR/build -lgwrt -lm -lpthread"
if ! $LINK -lpulse-simple 2>/dev/null; then
    $LINK 2>/dev/null || { echo "FAIL: link" >&2; exit 1; }
fi

./extern_demo > got.txt 2>&1
if diff -u "$FFI_DIR/expected.txt" got.txt; then
    echo "PASS  ffi extern_demo"
    exit 0
else
    echo "FAIL  ffi extern_demo (output mismatch above)" >&2
    exit 1
fi
