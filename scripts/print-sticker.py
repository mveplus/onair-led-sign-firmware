#!/usr/bin/env python3
"""Render a Wi-Fi join QR sticker for a freshly flashed device.

Workflow:
  1. Flash the firmware (USB still connected).
  2. Run this script once: it opens the device's USB-CDC, sends
     GET-STICKER, parses the JSON response, and writes a PNG containing
     a WIFI:T:WPA;S:...;P:...;; QR code that a phone camera auto-joins.

The on-device command is locked after the first successful STA join, so
this is a one-time bench step per device. Factory reset (5s BOOT hold)
regenerates the AP password and re-arms the command.

Requires: pyserial, qrcode[pil].
"""

import argparse
import json
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")

try:
    import qrcode
    from qrcode.image.pil import PilImage
except ImportError:
    sys.exit("qrcode[pil] is required: pip install 'qrcode[pil]'")


DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_BAUD = 115200
RESPONSE_TIMEOUT_SECS = 5.0


def request_sticker(port: str, baud: int) -> dict:
    with serial.Serial(port, baud, timeout=0.5) as ser:
        # Drain boot logs that may still be flushing through the buffer.
        time.sleep(0.5)
        ser.reset_input_buffer()
        ser.write(b"GET-STICKER\n")
        ser.flush()
        deadline = time.time() + RESPONSE_TIMEOUT_SECS
        while time.time() < deadline:
            line = ser.readline().decode(errors="replace").strip()
            if not line:
                continue
            if not line.startswith("STICKER:"):
                continue
            payload = line[len("STICKER:") :]
            try:
                return json.loads(payload)
            except json.JSONDecodeError as e:
                raise RuntimeError(f"invalid JSON from device: {e}: {payload!r}")
        raise TimeoutError(
            f"no STICKER: response within {RESPONSE_TIMEOUT_SECS}s"
        )


def make_wifi_uri(ssid: str, password: str) -> str:
    # Wi-Fi join URI per the de-facto standard used by phone cameras.
    # Escape backslash, semicolon, comma, colon, and quote with a backslash.
    def esc(s: str) -> str:
        return "".join("\\" + c if c in '\\;,":' else c for c in s)
    return f"WIFI:T:WPA;S:{esc(ssid)};P:{esc(password)};;"


def render_qr(uri: str, out_path: str) -> None:
    qr = qrcode.QRCode(
        version=None,
        error_correction=qrcode.constants.ERROR_CORRECT_M,
        box_size=10,
        border=4,
    )
    qr.add_data(uri)
    qr.make(fit=True)
    img = qr.make_image(
        image_factory=PilImage, fill_color="black", back_color="white"
    )
    img.save(out_path)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--port", default=DEFAULT_PORT,
        help="Serial port (default: %(default)s)",
    )
    ap.add_argument(
        "--baud", type=int, default=DEFAULT_BAUD,
        help="Baud rate (default: %(default)s)",
    )
    ap.add_argument(
        "--out", default=None,
        help="Output PNG path (default: sticker-<MAC>.png)",
    )
    args = ap.parse_args()

    try:
        info = request_sticker(args.port, args.baud)
    except (OSError, TimeoutError, RuntimeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if info.get("locked"):
        print(
            "device reports stk_lock=true — the AP password is hidden "
            "until factory reset (5s BOOT hold).",
            file=sys.stderr,
        )
        return 2

    mac = str(info["mac"]).replace(":", "").lower()
    ssid = info["ssid"]
    password = info["password"]
    uri = make_wifi_uri(ssid, password)
    out_path = args.out or f"sticker-{mac}.png"
    render_qr(uri, out_path)

    print(f"MAC      : {info['mac']}")
    print(f"SSID     : {ssid}")
    print(f"Password : {password}")
    print(f"QR URI   : {uri}")
    print(f"Saved    : {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
