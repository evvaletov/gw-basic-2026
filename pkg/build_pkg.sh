#!/bin/bash
# Build a FreeDOS-ready package for GW-BASIC 2026.
#
# Produces dist/gwbasic-<VERSION>.zip with the layout FreeDOS expects:
#   APPINFO/GWBASIC.LSM        (Linux Software Map metadata)
#   BIN/GWBASIC.EXE            (16-bit real-mode interpreter)
#   DOC/GWBASIC/README         (project README, CRLF)
#   DOC/GWBASIC/CHANGES        (version history, CRLF)
#   DOC/GWBASIC/LICENSE        (MIT, CRLF)
#   SOURCE/GWBASIC/<...>       (full source tree, optional)
#
# Run from the project root:  ./pkg/build_pkg.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_DIR"

VERSION=$(grep -oE '"[0-9]+\.[0-9]+\.[0-9]+"' include/gwbasic.h | tr -d '"')
[ -n "$VERSION" ] || { echo "Cannot determine version from include/gwbasic.h" >&2; exit 1; }

echo "==> Packaging GW-BASIC 2026 v$VERSION"

if [ ! -f gwbasic16.exe ] || [ src/main.c -nt gwbasic16.exe ]; then
    echo "==> Building gwbasic16.exe"
    ./build_dos.sh clean
    ./build_dos.sh 16
fi

STAGE=$(mktemp -d --tmpdir="$HOME" gw_pkg.XXXXXX)
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/APPINFO" "$STAGE/BIN" "$STAGE/DOC/GWBASIC" "$STAGE/SOURCE/GWBASIC"

# Metadata
cp pkg/GWBASIC.LSM "$STAGE/APPINFO/GWBASIC.LSM"
unix2dos -q "$STAGE/APPINFO/GWBASIC.LSM" 2>/dev/null \
    || sed -i 's/$/\r/' "$STAGE/APPINFO/GWBASIC.LSM"

# Binary
cp gwbasic16.exe "$STAGE/BIN/GWBASIC.EXE"

# Documentation (DOS line endings, 8.3-friendly names)
cp README.md "$STAGE/DOC/GWBASIC/README"
cp CHANGES.TXT "$STAGE/DOC/GWBASIC/CHANGES"
cp LICENSE "$STAGE/DOC/GWBASIC/LICENSE"
for f in "$STAGE/DOC/GWBASIC"/*; do
    unix2dos -q "$f" 2>/dev/null || sed -i 's/$/\r/' "$f"
done

# Source (so users can rebuild from the package).  Follow git's tracked-files
# list to avoid bundling build/, _build/, *.obj, etc.
git ls-files \
    | grep -v '^docs/_build/' \
    | grep -v '^build/' \
    | tar -cf - -T - \
    | tar -xf - -C "$STAGE/SOURCE/GWBASIC"

mkdir -p dist
ZIP="$PROJECT_DIR/dist/gwbasic-$VERSION.zip"
rm -f "$ZIP"
( cd "$STAGE" && zip -rq "$ZIP" APPINFO BIN DOC SOURCE )

echo
echo "==> Wrote $ZIP"
unzip -l "$ZIP" | tail -8
