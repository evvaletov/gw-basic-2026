#!/bin/bash
# Run gwbasic16.exe under DOSBox-X with tests/dos_smoke.bas and compare
# output against the golden file generated from the Linux interpreter.
# Verifies the BIOS-rendered TUI doesn't crash and that core features
# (arithmetic, strings, control flow, GOSUB, FOR/NEXT, DATA/READ, DEF FN,
# file I/O via OPEN/PRINT#) all work in the 16-bit DOS build.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
EXE="${PROJECT_DIR}/gwbasic16.exe"
SMOKE="${SCRIPT_DIR}/dos_smoke.bas"
EXPECTED="${SCRIPT_DIR}/expected/dos_smoke.expected"
DOSBOX_CONF="${SCRIPT_DIR}/dosbox-compat.conf"

if [ ! -f "$EXE" ]; then
    echo "ERROR: $EXE not found.  Run ./build_dos.sh 16 first." >&2
    exit 1
fi
if ! flatpak list --app 2>/dev/null | grep -q com.dosbox_x.DOSBox-X; then
    echo "ERROR: DOSBox-X flatpak not installed (com.dosbox_x.DOSBox-X)." >&2
    exit 1
fi

# DOSBox-X (flatpak) can only access $HOME, not /tmp.
WORK=$(mktemp -d --tmpdir="$HOME" gw_dos_smoke.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

cp "$EXE" "$WORK/GWBASIC.EXE"
cp "$SMOKE" "$WORK/SMOKE.BAS"
sed -i 's/$/\r/' "$WORK/SMOKE.BAS"

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    timeout 30 flatpak run com.dosbox_x.DOSBox-X \
        -conf "$DOSBOX_CONF" \
        -c "MOUNT C $WORK" \
        -c "C:" \
        -c "GWBASIC.EXE SMOKE.BAS" \
        -c "EXIT" \
        >/dev/null 2>&1

if [ ! -f "$WORK/OUT.TXT" ]; then
    echo "FAIL: gwbasic16.exe produced no OUT.TXT under DOSBox-X" >&2
    exit 1
fi

actual=$(mktemp)
normalized_expected=$(mktemp)
trap 'rm -rf "$WORK" "$actual" "$normalized_expected"' EXIT
sed 's/\r//g; s/[[:space:]]*$//' "$WORK/OUT.TXT" | sed '/^$/d' > "$actual"
sed 's/\r//g; s/[[:space:]]*$//' "$EXPECTED" | sed '/^$/d' > "$normalized_expected"

if diff -q "$normalized_expected" "$actual" >/dev/null; then
    echo "PASS: gwbasic16.exe smoke test matches golden output"
    exit 0
else
    echo "FAIL: gwbasic16.exe output differs from expected"
    diff -u "$normalized_expected" "$actual" | head -40
    exit 1
fi
