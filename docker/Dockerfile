FROM debian:bookworm-slim

ENV ARDUINO_DIRECTORIES_DATA=/opt/arduino
ENV ARDUINO_DIRECTORIES_USER=/opt/arduino/user
ENV ARDUINO_CLI_CONFIG_FILE=/opt/arduino/arduino-cli.yaml

RUN apt-get update \
  && apt-get install -y --no-install-recommends ca-certificates curl xz-utils \
  && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/arduino/user

ARG ARDUINO_CLI_VERSION=latest
RUN set -eux; \
  ARCH="$(dpkg --print-architecture)"; \
  case "$ARCH" in \
    amd64) ARDUINO_ARCH="Linux_64bit" ;; \
    arm64) ARDUINO_ARCH="Linux_ARM64" ;; \
    armhf) ARDUINO_ARCH="Linux_ARMv7" ;; \
    *) echo "Unsupported architecture: $ARCH" >&2; exit 1 ;; \
  esac; \
  if [ "$ARDUINO_CLI_VERSION" = "latest" ]; then \
    ARDUINO_CLI_VERSION="$(curl -fsSL https://api.github.com/repos/arduino/arduino-cli/releases/latest | sed -n 's/.*\"tag_name\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p')"; \
  fi; \
  ARDUINO_VER="${ARDUINO_CLI_VERSION#v}"; \
  curl -fsSL -o /tmp/arduino-cli.tar.gz "https://github.com/arduino/arduino-cli/releases/download/${ARDUINO_CLI_VERSION}/arduino-cli_${ARDUINO_VER}_${ARDUINO_ARCH}.tar.gz"; \
  tar -xzf /tmp/arduino-cli.tar.gz -C /usr/local/bin arduino-cli; \
  rm -f /tmp/arduino-cli.tar.gz

# Configure Arduino CLI and install ESP32 core + required libraries
RUN arduino-cli config init --overwrite \
  && arduino-cli config set board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json \
  && arduino-cli core update-index \
  && arduino-cli core install esp32:esp32 \
  && arduino-cli lib install "ESP Async WebServer@3.9.4" "AsyncTCP@3.4.10" "ArduinoJson@7.4.2"

WORKDIR /workspace
