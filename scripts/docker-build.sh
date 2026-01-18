#!/usr/bin/env bash
set -euo pipefail

IMAGE=${IMAGE:-onair-led-sign-firmware-arduino}

docker build -t "$IMAGE" -f docker/Dockerfile .

docker run --rm \
  -v "$PWD":/workspace \
  -w /workspace \
  -e FQBN \
  -e SKETCH \
  -e OUT_DIR \
  "$IMAGE" \
  /workspace/scripts/build.sh
