#!/usr/bin/env python3
"""Render a Wi-Fi join QR sticker for a freshly flashed device.

Workflow:
  1. Flash the firmware (USB still connected).
  2. Run this script once: it opens the device's USB-CDC, sends
     GET-STICKER, parses the JSON response, and writes a PNG containing
     a WIFI:T:WPA;S:...;P:...;; QR code that a phone camera auto-joins.

The on-device GET-STICKER command is locked once the device has joined
its first STA network. A normal 5 s BOOT factory reset does NOT clear
that lock by design — sticker stays valid across reset. To rotate the
AP password and re-print a sticker, run with --reset: that sends
RESET-MFG over Serial, the device wipes its "mfg" NVS namespace
(ap_pass + stk_lock) and reboots with a fresh password.

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


def request_reset_mfg(port: str, baud: int, settle_secs: float = 6.0) -> None:
    """Send RESET-MFG. The device wipes the "mfg" NVS namespace (ap_pass +
    stk_lock) and reboots; the USB-CDC drops and re-enumerates during the
    restart, so callers should retry their next read for a few seconds.
    """
    with serial.Serial(port, baud, timeout=0.5) as ser:
        time.sleep(0.3)
        ser.reset_input_buffer()
        ser.write(b"RESET-MFG\n")
        ser.flush()
        deadline = time.time() + 2.0
        ack = False
        while time.time() < deadline:
            line = ser.readline().decode(errors="replace").strip()
            if line and "MFG-RESET" in line:
                ack = True
                break
    if not ack:
        raise RuntimeError(
            "device did not acknowledge RESET-MFG within 2 s "
            "(is this the right firmware build?)"
        )
    # Give the chip time to reboot and re-enumerate USB-CDC.
    time.sleep(settle_secs)


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
    ap.add_argument(
        "--reset", action="store_true",
        help=(
            "Send RESET-MFG before reading. Rotates the AP password and "
            "unlocks the GET-STICKER response. Use when the device "
            "reports stk_lock=true and you need a new sticker. NOTE: any "
            "previously printed sticker becomes invalid."
        ),
    )
    args = ap.parse_args()

    if args.reset:
        print(
            f"sending RESET-MFG to {args.port} (device will reboot)…",
            file=sys.stderr,
        )
        try:
            request_reset_mfg(args.port, args.baud)
        except (OSError, TimeoutError, RuntimeError) as e:
            print(f"error during RESET-MFG: {e}", file=sys.stderr)
            return 1
        print(
            "device rebooted; reading fresh sticker info "
            "(have to be quick — STA join re-engages the lock)…",
            file=sys.stderr,
        )

    # After a reset the USB-CDC takes a few seconds to re-enumerate; retry
    # the open a handful of times before giving up.
    attempts = 8 if args.reset else 1
    info = None
    last_err: Exception | None = None
    for i in range(attempts):
        try:
            info = request_sticker(args.port, args.baud)
            break
        except (OSError, TimeoutError, RuntimeError) as e:
            last_err = e
            if i < attempts - 1:
                time.sleep(1)
    if info is None:
        print(f"error: {last_err}", file=sys.stderr)
        return 1

    if info.get("locked"):
        print(
            "device reports stk_lock=true — first STA join already "
            "happened. Re-run with --reset to rotate the AP password "
            "and unlock the read.\n"
            "(--reset invalidates any previously printed sticker.)",
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
