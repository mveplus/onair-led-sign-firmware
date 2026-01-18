#!/usr/bin/env bash
set -euo pipefail

SKETCH=${SKETCH:-onair-led-sign-firmware.ino}
FQBN=${FQBN:-esp32:esp32:xiao_esp32c6}
OUT_DIR=${OUT_DIR:-build}

mkdir -p "$OUT_DIR"

arduino-cli compile \
  --fqbn "$FQBN" \
  --export-binaries \
  --output-dir "$OUT_DIR" \
  "$SKETCH"

shopt -s nullglob
bins=("$OUT_DIR"/*.bin)
if [ ${#bins[@]} -eq 0 ]; then
  echo "No .bin files found in $OUT_DIR" >&2
  exit 1
fi

(
  cd "$OUT_DIR"
  sha1sum *.bin > sha1sums.txt
)
