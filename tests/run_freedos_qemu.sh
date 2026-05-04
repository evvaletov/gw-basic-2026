#!/bin/bash
# Launch a FreeDOS 1.4 QEMU VM with gwbasic16.exe and the smoke test on a
# FAT disk image, for manual verification of the BIOS-rendered TUI.
#
# Usage:  bash tests/run_freedos_qemu.sh
#
# The script is *not* a self-contained automated smoke test — DOS doesn't
# pipe console input cleanly enough through QEMU's -serial stdio for that
# to be reliable, and modern qemu-kvm builds don't expose -fda on the
# default machine type.  Use tests/run_dos_smoke.sh (DOSBox-X) for the
# automated smoke; this script is for going through the manual TUI
# checklist in docs/getting-started.md against bare FreeDOS.
#
# Once FreeDOS boots, press a key to skip the boot menu (or wait for the
# default), then at the C:\> prompt:
#
#     C:\> D:
#     D:\> DIR                       (sanity-check the FAT image is mounted)
#     D:\> GWBASIC.EXE SMOKE.BAS     (runs the non-interactive smoke)
#     D:\> TYPE OUT.TXT              (compare against tests/expected/dos_smoke.expected)
#     D:\> GWBASIC.EXE               (no args -- exercises the TUI editor)
#
# Quit QEMU: Ctrl-Alt-G then close the window, or in -nographic mode use
# the QEMU monitor (Ctrl-A then X).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
EXE="$PROJECT_DIR/gwbasic16.exe"
SMOKE="$SCRIPT_DIR/dos_smoke.bas"
FREEDOS_IMG="${FREEDOS_IMG:-$HOME/DOS/images/freedos.qcow2}"
QEMU="${QEMU:-/usr/libexec/qemu-kvm}"
DATA_IMG="$HOME/.cache/gw-basic-2026/freedos_d.img"

if [ ! -x "$EXE" ]; then
    echo "ERROR: $EXE not found.  Run ./build_dos.sh 16 first." >&2
    exit 1
fi
if [ ! -f "$FREEDOS_IMG" ]; then
    echo "ERROR: FreeDOS qcow2 not found at $FREEDOS_IMG." >&2
    echo "Override with FREEDOS_IMG=/path/to/freedos.qcow2" >&2
    exit 1
fi
if [ ! -x "$QEMU" ]; then
    echo "ERROR: qemu-kvm not found at $QEMU.  Override with QEMU=/path/to/qemu" >&2
    exit 1
fi

echo "==> Building FAT data image at $DATA_IMG"
mkdir -p "$(dirname "$DATA_IMG")"
dd if=/dev/zero of="$DATA_IMG" bs=1M count=8 status=none
mkfs.vfat "$DATA_IMG" >/dev/null
mcopy -i "$DATA_IMG" "$EXE"   ::GWBASIC.EXE
mcopy -i "$DATA_IMG" "$SMOKE" ::SMOKE.BAS
sed 's/$/\r/' "$SMOKE" > /tmp/_smoke_crlf.bas
mcopy -i "$DATA_IMG" -o /tmp/_smoke_crlf.bas ::SMOKE.BAS
rm -f /tmp/_smoke_crlf.bas
mdir -i "$DATA_IMG"

echo
echo "==> Launching FreeDOS QEMU.  See header of $0 for the manual test sequence."
echo "==> Quit: Ctrl-Alt-G, close window. Press Enter to continue."
read -r

exec "$QEMU" \
    -machine pc \
    -cpu max \
    -m 32 \
    -hda "$FREEDOS_IMG" \
    -hdb "$DATA_IMG" \
    -nic none \
    -boot c \
    -display gtk \
    -rtc base=localtime
