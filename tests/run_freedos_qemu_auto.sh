#!/bin/bash
# Portable FreeDOS smoke checks for both DOS builds; see docs/development.md.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec python3 "$SCRIPT_DIR/run_dos_runtime.py" "$@"
