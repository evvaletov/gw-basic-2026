#!/bin/bash
# build_dos.sh -- Cross-compile GW-BASIC 2026 to DOS using OpenWatcom V2.
#
# Usage:
#   ./build_dos.sh                  # 16-bit real-mode, produces gwbasic16.exe
#   ./build_dos.sh 32               # 32-bit DOS/4GW, produces gwbasic.exe
#   ./build_dos.sh clean            # remove .obj files and DOS executables
#
# OpenWatcom's wmake on Linux struggles with the .c.obj implicit rule for
# subdirectory paths, so this script invokes wcc/wcc386 directly.  On a
# Windows or DOS host, use Makefile.dos / Makefile.dos16 with wmake instead.
#
# Requires:  OpenWatcom V2 with $WATCOM and binl64 (or binl) on PATH.
#            Source ~/openwatcom-v2/setvars.sh first if not already.

set -e

if [ -z "$WATCOM" ]; then
    if [ -f "$HOME/openwatcom-v2/setvars.sh" ]; then
        . "$HOME/openwatcom-v2/setvars.sh"
    else
        echo "Error: WATCOM not set and ~/openwatcom-v2/setvars.sh not found." >&2
        echo "Install OpenWatcom V2 and set WATCOM to its root." >&2
        exit 1
    fi
fi

INTERP_C=(
    src/main.c src/tokens.c src/tokenizer.c src/error.c
    src/eval.c src/interp.c src/vars.c src/arrays.c
    src/input.c src/math_int.c src/math_float.c
    src/math_transcend.c src/strings.c src/print.c
    src/fileio.c src/program_io.c src/print_using.c
    src/graphics.c src/virmem.c src/portio.c src/strpool.c
    src/sound.c src/tui.c platform/hal_dos.c
)

case "${1:-16}" in
    clean)
        rm -f src/*.obj platform/*.obj gwbasic.exe gwbasic16.exe gwbascom.exe gwrt.lib .dos_build_mode
        echo "cleaned"
        exit 0
        ;;
    16)
        CC=wcc
        CFLAGS="-bt=dos -mm -ox -w4 -zq -za99 -Iinclude -D__MSDOS__"
        EXE=gwbasic16.exe
        LINK_SYSTEM="dos option stack=8192"
        ;;
    32)
        CC=wcc386
        CFLAGS="-bt=dos -mf -ox -w4 -zq -za99 -Iinclude -D__MSDOS__"
        EXE=gwbasic.exe
        LINK_SYSTEM="dos4g"
        ;;
    *)
        echo "Usage: $0 [16|32|clean]" >&2
        exit 1
        ;;
esac

# Track which mode the .obj files were built for; clean if it differs.  16-bit
# .obj from wcc and 32-bit .obj from wcc386 share names but cannot mix in one
# link.
MODE_STAMP=.dos_build_mode
PREV_MODE=""
[ -f "$MODE_STAMP" ] && PREV_MODE=$(cat "$MODE_STAMP")
if [ -n "$PREV_MODE" ] && [ "$PREV_MODE" != "$1" ] && [ "$PREV_MODE" != "${1:-16}" ]; then
    echo "  -- previous build was $PREV_MODE, cleaning"
    rm -f src/*.obj platform/*.obj
fi
echo "${1:-16}" > "$MODE_STAMP"

OBJS=()
for c in "${INTERP_C[@]}"; do
    obj="${c%.c}.obj"
    OBJS+=("$obj")
    if [ "$c" -nt "$obj" ] || [ ! -f "$obj" ]; then
        printf "  CC  %s\n" "$c"
        $CC $CFLAGS -fo="$obj" "$c"
    fi
done

printf "  LD  %s\n" "$EXE"
LINK_DIR=$(dirname "$0")/.link_dir
mkdir -p "$LINK_DIR"
LINK_SCRIPT="$LINK_DIR/link.lnk"
{
    echo "system $LINK_SYSTEM"
    echo "name $EXE"
    for o in "${OBJS[@]}"; do
        echo "file $o"
    done
} > "$LINK_SCRIPT"
wlink @"$LINK_SCRIPT"
rm -rf "$LINK_DIR"

ls -la "$EXE"
