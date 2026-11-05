#!/bin/bash
# Curated reference-output and interpreter/compiler parity checks.
set -eu
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec python3 "$SCRIPT_DIR/run_corpus.py" "$@"
