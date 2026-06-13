#!/bin/bash
# Automated headless FreeDOS smoke test for gwbasic16.exe under QEMU.
#
# Unlike tests/run_freedos_qemu.sh (manual, interactive TUI checklist), this
# runs fully automated: it overlays the FreeDOS image (never mutating the
# original), stages the interpreter + a SYSTEM-terminated copy of the smoke on
# C:, injects the run + poweroff into the image's startup batch, boots headless,
# then reads OUT.TXT back and diffs it against the golden file.
#
# This is a LOCAL-DEV smoke (not a GitHub-CI job): it needs qemu, a FreeDOS
# qcow2 image, mtools, the nbd kernel module, and passwordless sudo (to attach
# the qcow2 via qemu-nbd and mount its partition). For CI use the DOSBox-X path,
# tests/run_dos_smoke.sh, instead.  It complements that path by exercising the
# binary on a real FreeDOS install rather than DOSBox-X's emulation.
#
# Overridable via env: FREEDOS_IMG, QEMU, BOOT_TIMEOUT.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
EXE="$PROJECT_DIR/gwbasic16.exe"
SMOKE="$SCRIPT_DIR/dos_smoke.bas"
EXPECTED="$SCRIPT_DIR/expected/dos_smoke.expected"
FREEDOS_IMG="${FREEDOS_IMG:-$HOME/DOS/images/freedos.qcow2}"
QEMU="${QEMU:-/usr/libexec/qemu-kvm}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-75}"

skip() { echo "SKIP: $1"; exit 0; }

[ -x "$EXE" ]         || { echo "ERROR: $EXE not found. Run ./build_dos.sh 16 first." >&2; exit 1; }
[ -f "$FREEDOS_IMG" ] || skip "FreeDOS image not found at $FREEDOS_IMG (set FREEDOS_IMG=...)"
[ -x "$QEMU" ]        || skip "qemu not found at $QEMU (set QEMU=...)"
command -v qemu-nbd >/dev/null || skip "qemu-nbd not installed"
command -v mcopy    >/dev/null || skip "mtools not installed"
sudo -n true 2>/dev/null       || skip "passwordless sudo required (qemu-nbd + mount)"

WORK=$(mktemp -d --tmpdir="$HOME" gw_qemu_smoke.XXXXXX)
OVERLAY="$WORK/boot.qcow2"
MNT="$WORK/mnt"; mkdir -p "$MNT"
NBD=""
cleanup() {
    mountpoint -q "$MNT" 2>/dev/null && sudo umount "$MNT" 2>/dev/null
    [ -n "$NBD" ] && sudo qemu-nbd -d "$NBD" >/dev/null 2>&1
    rm -rf "$WORK"
}
trap cleanup EXIT

# Find and connect a free /dev/nbdN to the overlay; sets $NBD and mounts $MNT.
nbd_attach() {
    local n
    for n in $(seq 0 15); do
        if [ ! -e "/sys/block/nbd$n/pid" ] \
           && sudo qemu-nbd -c "/dev/nbd$n" "$OVERLAY" 2>/dev/null; then
            NBD="/dev/nbd$n"; break
        fi
    done
    [ -n "$NBD" ] || { echo "ERROR: no free nbd device" >&2; exit 1; }
    sleep 1
    sudo mount "${NBD}p1" "$MNT" 2>/dev/null || sudo mount "$NBD" "$MNT"
}
nbd_detach() { sudo umount "$MNT"; sudo qemu-nbd -d "$NBD" >/dev/null 2>&1; NBD=""; }

# Disposable overlay so the base image is never modified.
qemu-img create -f qcow2 -b "$FREEDOS_IMG" -F qcow2 "$OVERLAY" >/dev/null
sudo modprobe nbd max_part=8 2>/dev/null
nbd_attach

# Stage the interpreter + a SYSTEM-terminated smoke (so it returns to DOS for a
# clean poweroff) on C:.
sudo cp "$EXE" "$MNT/GWBASIC.EXE"
sed 's/^\([0-9][0-9]*\) END\b/\1 SYSTEM/; s/$/\r/' "$SMOKE" | sudo tee "$MNT/SMOKE.BAS" >/dev/null
sudo rm -f "$MNT/OUT.TXT"

# Inject the run + poweroff into the real startup batch.  FreeDOS runs the file
# named by the shell's /P= in FDCONFIG.SYS/CONFIG.SYS (commonly FDAUTO.BAT, not
# AUTOEXEC.BAT); parse it, with sensible fallbacks.
cfg=""
for c in FDCONFIG.SYS CONFIG.SYS; do [ -f "$MNT/$c" ] && cfg="$MNT/$c" && break; done
startup=""
if [ -n "$cfg" ]; then
    p=$(tr -d '\r' < "$cfg" | grep -ioE '/P=[A-Za-z]:\\[A-Za-z0-9._]+' | head -1 | sed -E 's#.*\\##')
    [ -n "$p" ] && [ -f "$MNT/$p" ] && startup="$MNT/$p"
fi
[ -z "$startup" ] && for b in FDAUTO.BAT AUTOEXEC.BAT; do [ -f "$MNT/$b" ] && startup="$MNT/$b" && break; done
[ -n "$startup" ] || { echo "ERROR: no startup batch found on image" >&2; exit 1; }
printf 'C:\r\nGWBASIC.EXE SMOKE.BAS\r\nFDAPM POWEROFF\r\n' | sudo tee -a "$startup" >/dev/null

# Auto-select the boot menu default (a ,0 timeout waits forever with no keyboard).
[ -n "$cfg" ] && sudo sed -i -E 's/^(MENUDEFAULT=[0-9]+),[0-9]+/\1,2/' "$cfg"

nbd_detach

# Boot headless.  A clean exit means the smoke ran and FDAPM powered off; the
# timeout is a safety net (OUT.TXT is written before any hang at the Ok prompt).
timeout "$BOOT_TIMEOUT" "$QEMU" -machine pc -cpu max -m 32 \
    -hda "$OVERLAY" -nic none -boot c \
    -display none -serial null -monitor none -no-reboot >/dev/null 2>&1

# Read OUT.TXT back from C:.
nbd_attach
sudo cp "$MNT/OUT.TXT" "$WORK/OUT.TXT" 2>/dev/null || { echo "FAIL: smoke produced no OUT.TXT"; exit 1; }
nbd_detach

norm() { sed 's/\r//g; s/[[:space:]]*$//' "$1" | sed '/^$/d'; }
if diff -q <(norm "$EXPECTED") <(norm "$WORK/OUT.TXT") >/dev/null; then
    echo "PASS: gwbasic16.exe smoke matches golden output (FreeDOS / QEMU)"
    exit 0
else
    echo "FAIL: gwbasic16.exe output differs from expected"
    diff -u <(norm "$EXPECTED") <(norm "$WORK/OUT.TXT") | head -40
    exit 1
fi
