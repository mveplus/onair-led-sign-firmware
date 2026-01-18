FROM arduino/arduino-cli:latest

ENV ARDUINO_DIRECTORIES_DATA=/opt/arduino
ENV ARDUINO_DIRECTORIES_USER=/opt/arduino/user
ENV ARDUINO_CLI_CONFIG_FILE=/opt/arduino/arduino-cli.yaml

RUN mkdir -p /opt/arduino/user

# Configure Arduino CLI and install ESP32 core + required libraries
RUN arduino-cli config init --overwrite \
  && arduino-cli config set board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json \
  && arduino-cli core update-index \
  && arduino-cli core install esp32:esp32 \
  && arduino-cli lib install "ESP Async WebServer@3.9.4" "AsyncTCP@3.4.10" "ArduinoJson@7.4.2"

WORKDIR /workspace
