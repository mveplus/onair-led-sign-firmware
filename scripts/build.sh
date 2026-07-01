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
cp -f *.h *.cpp "$SKETCH_DIR/" 2>/dev/null || true
# Ship the custom partition table (reclaims the unused SPIFFS into the two
# OTA app slots, ~1.94MB each). Requires PartitionScheme=custom below.
cp -f partitions.csv "$SKETCH_DIR/" 2>/dev/null || true

GIT_SHA="${GIT_SHA:-$(git rev-parse --short HEAD 2>/dev/null || true)}"
[ -z "$GIT_SHA" ] && GIT_SHA="nogit"
BUILD_DATE="${BUILD_DATE:-$(date -u +%Y-%m-%d)}"
# FW_VERSION is honored as-is if the caller already set it (CI passes the
# tag name for v* builds, the git-described SHA for branch builds). Only
# fall back to the date+sha pattern when nothing was provided — that's
# what local hand-runs of build.sh hit.
FW_VERSION="${FW_VERSION:-${BUILD_DATE}+${GIT_SHA}}"

IFS=',' read -r -a fqbn_list <<< "$FQBN"
# App-slot size from partitions.csv (0x1F0000) — used as an accurate
# over-size guard, since the "custom" scheme's default maximum_size is 16MB.
APP_MAX_SIZE="${APP_MAX_SIZE:-2031616}"

for fqbn in "${fqbn_list[@]}"; do
  fqbn_trimmed="$(echo "$fqbn" | xargs)"
  [ -z "$fqbn_trimmed" ] && continue
  echo "Building for $fqbn_trimmed"
  # The esp32 core auto-applies the sketch-local partitions.csv (it sets
  # build.custom_partitions when present); we only override maximum_size so
  # the over-size guard reflects the real ~1.94MB app slot, not the stock
  # scheme's 1.28MB (which would false-fail at 99%).
  arduino-cli compile \
    --fqbn "$fqbn_trimmed" \
    --build-property "compiler.cpp.extra_flags=-DFW_VERSION=\"${FW_VERSION}\" -DENABLE_AWS_IOT=${ENABLE_AWS_IOT:-1}" \
    --build-property "upload.maximum_size=${APP_MAX_SIZE}" \
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
