# ESP32-C6 On-Air LED Sign Firmware

Firmware for an ESP32‑C6 board (default: Seeed XIAO ESP32‑C6) that provides a captive setup portal, a small HTTP API, OTA updates, and a configurable output pin with optional breathing (PWM) mode.

This repo contains a single Arduino sketch plus an API contract document.

## Features

- Captive setup portal for Wi‑Fi provisioning (AP+STA).
- HTTP API to read status and control an output pin.
- OTA firmware updates at `/update`.
- mDNS advertising of the configured hostname.
- Factory reset via BOOT long‑press (5s).
- Optional breathing output mode using PWM.

## Project Layout

- `esp32c6-led-sign-firmware.ino` — main firmware sketch
- `API.md` — API contract and endpoints

## Hardware Defaults

- Board name: [`XIAO-ESP32-C6`](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/#hardware-overview) or [Beetle ESP32-C6](https://wiki.dfrobot.com/SKU_DFR1117_Beetle_ESP32_C6?Board%20Overview#Pin%20Diagram)
- Built‑in LED: `LED_BUILTIN` (defaults to GPIO8 if not defined by core)
- BOOT button pin: GPIO9
- Default output pin: GPIO6
- LED polarity: active HIGH by default (configurable)

## Wiring Diagram (FGP30N06L, Same USB-C Supply)

Low-side switch using an N-channel MOSFET. The LED sign and ESP32‑C6 board share the same USB‑C 5V supply.

```
USB-C 5V
  +5V --------------------+---------------------+
          |                                     |
LED SIGN (+)                                 ESP32-C6 5V      
                                                |
LED SIGN (-) ----> DRAIN (FGP30N06L) SOURCE <---+---- GND (common)

ESP32-C6 GPIO (Output pin, default GPIO6)
  |
  +--[100-220R]--> GATE (FGP30N06L)
  |
 [10k]
  |
 GND
```

Notes:

- The ESP32‑C6 is 3.3V logic. FGP30N06L is suitable and enhances at 3.3V gate drive; it is a logic‑level N-Channel MOSFET.
- The gate pulldown (10k) keeps the MOSFET off during boot/reset.
- Use a small series gate resistor (100–220 ohm) to reduce ringing.
- If your LED sign is inductive, add a flyback diode across the load (anode to MOSFET drain, cathode to +5V).
- Always share ground between the board, MOSFET, and LED sign power.

## Setup Workflow (First Boot)

1. Device boots and tries saved Wi‑Fi credentials.
2. If none or connection fails, it starts a setup AP + captive portal.
3. Join the AP and configure Wi‑Fi, hostname, output pin, auth, etc.

### Setup AP Details

- SSID: `C6-SETUP-<MAC12>` (12 hex characters from the device MAC).
- Password:
  - Generated once and stored in Preferences.
  - Format: `<word>-<word>-NN` (easy to type, WPA2/WPA3 compliant).
  - Can be set to empty for an open AP (via setup UI).
- Captive portal auto‑scans networks; manual/hidden SSID supported.

## Serial Setup Log (Find SSID + QR)

After flashing, you can read the setup AP SSID/password and QR URL over serial.

1. Connect over USB.
2. Start the monitor:

```bash
arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200
```

Alternative: use `screen` (no auto-reset toggles):

```bash
screen /dev/ttyACM0 115200
```

Exit `screen` with `Ctrl-A` then `K` (confirm with `y`).

Example output (trimmed):

```
Connecting to /dev/ttyACM0. Press CTRL-C to exit.
AP SSID (full): C6-SETUP-0AFEFF043254
AP PASS: ridge-olive-99
AP IP:   192.168.4.1
WiFi QR payload (copy-paste into QR generator):
WIFI:T:WPA;S:C6-SETUP-0AFEFF043254;P:ridge-olive-99;H:false;;
WiFi QR URL (QuickChart, size=500): https://quickchart.io/qr?text=WIFI%3AT%3AWPA%3BS%3AC6-SETUP-0AFEFF043254%3BP%3Aridge-olive-99%3BH%3Afalse%3B%3B&size=500
```

Use the SSID/password to join the setup AP, or open the QR URL on your phone to connect quickly.

Tip: After reflashing firmware, it’s often best to **factory reset** (hold BOOT for ~5 seconds) so the device clears old Wi‑Fi/settings and restarts cleanly.

### Setup AP Timeout

- Default timeout: 10 minutes.
- If still in setup mode after timeout, the device reboots and retries STA.

## Flashing Firmware

You can either flash the provided binaries from `build/` or upload a locally built sketch with [`arduino-cli`](https://github.com/arduino/arduino-cli) or [`esptool.py`](https://github.com/espressif/esptool)

### Option A: Flash Provided Binaries (from `build/`)

1. Put the board in download mode if needed (hold BOOT, tap RESET).
2. Connect the board over USB and note the serial port (example: `/dev/ttyACM0`).
3. Use `esptool.py` to write the merged image:

```bash
esptool.py --chip esp32c6 --port /dev/ttyACM0 --baud 460800 write_flash 0x0 build/onair-led-sign-firmware.ino.merged.bin
```

If you need separate images instead of the merged binary, use these offsets:

```bash
esptool.py --chip esp32c6 --port /dev/ttyACM0 --baud 460800 write_flash \
  0x0 build/onair-led-sign-firmware.ino.bootloader.bin \
  0x8000 build/onair-led-sign-firmware.ino.partitions.bin \
  0x10000 build/onair-led-sign-firmware.ino.bin
```

### Option B: Build + Upload with `arduino-cli`

1. Build (optional if you already have `build/`):

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c6 .
```

2. Upload using the provided build output directory:

```bash
arduino-cli upload --fqbn esp32:esp32:esp32c6 -p /dev/ttyACM0 --input-dir build
```

## Connected Mode UI

When connected to Wi‑Fi (STA):

- Root page shows status, toggle controls, breathing settings, and OTA entry.
- mDNS is advertised as `http://<hostname>.local/`.

## First Login & Token Generation

1. After the first successful STA connection, open the device UI:
   - `http://<device-ip>/` or `http://<hostname>.local/`
2. When prompted, log in with Basic Auth:
   - Default: `admin` / `esp32c6` (change in setup portal → Advanced).
3. The API token is generated automatically on that first STA connection.
4. You can view/copy the token on the connected UI page under **API access**.

## Output Control

Output modes:

- `off`
- `on`
- `breathing` (PWM; configurable period/min/max)

Breathing defaults:

- Period: 3000 ms
- Min: 5%
- Max: 100%

Allowed ranges:

- `period_ms`: 500–10000
- `min_pct`: 1–99
- `max_pct`: 1–100 (must be > min)

## Authentication

All API and OTA endpoints require auth:

- HTTP Basic Auth using configured admin user/password.
- Or API token via:
  - `X-API-Token: <token>`
  - `Authorization: Bearer <token>`
  - `?token=<token>`

Token behavior:

- Generated after the first successful STA connection.
- Stored in Preferences and displayed on the connected UI page.

Default credentials:

- Username: `admin`
- Password: `esp32c6`
- Change these in the setup portal under **Advanced** (Admin user/password fields).

## API

See `API.md` for full details.

Quick endpoints:

- `GET /api/status`
- `GET /api/set?state=0|1`
- `GET /api/mode?mode=off|on|breathing[&period_ms=...&min_pct=...&max_pct=...]`
- `GET /api/config`

### Example `curl` Commands

Replace `<ip>` with the device IP and `<token>` with your API token.

```bash
# Status
curl -H "X-API-Token: <token>" http://<ip>/api/status

# Turn output ON
curl -H "X-API-Token: <token>" "http://<ip>/api/set?state=1"

# Turn output OFF
curl -H "X-API-Token: <token>" "http://<ip>/api/set?state=0"

# Set breathing mode (3s, 5% -> 100%)
curl -H "X-API-Token: <token>" "http://<ip>/api/mode?mode=breathing&period_ms=3000&min_pct=5&max_pct=100"

# Read stored config
curl -H "X-API-Token: <token>" http://<ip>/api/config
```

## OTA Updates

- `GET /update` serves the upload form (STA mode only).
- `POST /update` accepts a compiled `.bin`, then reboots.
- OTA is disabled while in setup (AP) mode.

## Factory Reset

- Hold BOOT for ~5 seconds.
- LED blinks while holding, then resets stored config and reboots.

## Build & Flash (Arduino IDE)

1. Open `esp32c6-led-sign-firmware.ino`.
2. Select an ESP32‑C6 board (tested with XIAO ESP32‑C6).
3. Install required libraries:
   - ESPAsyncWebServer v3.9.4
   - AsyncTCP v3.4.10 (Arduino IDE built‑in via Library Manager)
   - ArduinoJson v6.x (Arduino IDE built‑in via Library Manager)
4. Compile and upload.

## Docker Build (arduino-cli)

Build binaries locally in Docker and write outputs to `build/` with SHA1 signatures.

```bash
./scripts/docker-build.sh
```

Override the board(s) if needed (comma-separated FQBNs):

```bash
FQBN=esp32:esp32:dfrobot_beetle_esp32c6,esp32:esp32:XIAO_ESP32C6 ./scripts/docker-build.sh
```

## Optional BLE Provisioning

BLE provisioning is compile‑time gated:

- Set `ENABLE_BLE_PROV` to `1`.
- Requires `WiFiProv.h` availability in your core/toolchain.
- AP portal remains the primary provisioning path.

## Notes / Troubleshooting

- If the output pin is set to the built‑in LED, the LED polarity setting matters.
- PWM breathing requires a PWM‑capable GPIO.
- If you see “OTA disabled in setup mode”, connect the device to Wi‑Fi first.
