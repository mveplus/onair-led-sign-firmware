#!/usr/bin/env bash
set -euo pipefail

IMAGE=${IMAGE:-onair-led-sign-firmware-arduino}

docker build -t "$IMAGE" -f docker/Dockerfile .

docker run --rm \
  -v "$PWD":/workspace:Z \
  -w /workspace \
  -e FQBN \
  -e SKETCH \
  -e OUT_DIR \
  -e GIT_SHA \
  -e BUILD_DATE \
  -e FW_VERSION \
  -e ENABLE_AWS_IOT \
  "$IMAGE" \
  bash /workspace/scripts/build.sh
