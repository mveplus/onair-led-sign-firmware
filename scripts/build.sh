#!/usr/bin/env bash
set -euo pipefail

SKETCH=${SKETCH:-onair-led-sign-firmware.ino}
FQBN=${FQBN:-esp32:esp32:XIAO_ESP32C6}
#FQBN=${FQBN:-esp32:esp32:dfrobot_beetle_esp32c6,esp32:esp32:XIAO_ESP32C6}
OUT_DIR=${OUT_DIR:-build}

mkdir -p "$OUT_DIR"

SKETCH_BASENAME="$(basename "$SKETCH")"
SKETCH_NAME="${SKETCH_BASENAME%.ino}"
SKETCH_DIR="$OUT_DIR/$SKETCH_NAME"

mkdir -p "$SKETCH_DIR"
cp -f "$SKETCH" "$SKETCH_DIR/$SKETCH_BASENAME"

GIT_SHA="$(git rev-parse --short HEAD 2>/dev/null || true)"
[ -z "$GIT_SHA" ] && GIT_SHA="nogit"
BUILD_DATE="$(date -u +%Y-%m-%d)"
FW_VERSION="${BUILD_DATE}+${GIT_SHA}"

IFS=',' read -r -a fqbn_list <<< "$FQBN"
for fqbn in "${fqbn_list[@]}"; do
  fqbn_trimmed="$(echo "$fqbn" | xargs)"
  [ -z "$fqbn_trimmed" ] && continue
  echo "Building for $fqbn_trimmed"
  arduino-cli compile \
    --fqbn "$fqbn_trimmed" \
    --build-property "compiler.cpp.extra_flags=-DFW_VERSION=\"${FW_VERSION}\"" \
    --export-binaries \
    --output-dir "$OUT_DIR" \
    "$SKETCH_DIR"
done

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
