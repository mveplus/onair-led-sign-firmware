# Portal API Contract

This document describes the HTTP API exposed by
`onair-led-sign-firmware.ino`. All endpoints return JSON unless otherwise
noted.

Authentication
--------------
API and OTA endpoints require auth:

- Basic auth using the configured admin user/password.
- Token via one of:
  - `X-API-Token: <token>`
  - `Authorization: Bearer <token>`
  - `?token=<token>`

Token generation:
- The token is created after the first successful STA connection and stored in prefs.
- It is shown on the connected UI page (not in the setup portal).

Endpoints
---------

GET /api/status
- Returns current network/device state.
- Response fields:
  - `ok` (bool)
  - `mode` ("ap" | "sta")
  - `ip` (string)
  - `ssid` (string)
  - `hostname` (string)
  - `out_pin` (int)
  - `led_active_high` (bool)
  - `output_mode` ("off" | "on" | "breathing")
  - `br_period_ms` (int)
  - `br_min_pct` (int)
  - `br_max_pct` (int)
  - `state` (bool, true when output is ON)
  - `rssi` (int, only in STA mode)

GET /api/set?state=0|1
- Sets output mode to OFF (0) or ON (1).
- Response fields: `ok`, `state`.

GET /api/mode?mode=off|on|breathing[&period_ms=500..10000][&min_pct=1..99][&max_pct=1..100]
- Sets output mode and optional breathing timing.
- `period_ms`, `min_pct`, and `max_pct` are only used when provided.
- Response fields: `ok`, `output_mode`, `br_period_ms`, `br_min_pct`, `br_max_pct`.
- Errors:
  - `400` if `period_ms`, `min_pct`, or `max_pct` are out of range.
  - `400` if `max_pct` is not greater than `min_pct`.

POST /api/pin?out=0..48
POST /api/pin?usebl=1
- Changes the output GPIO at runtime without re-running setup.
- `out` selects a raw GPIO (e.g. `18` = XIAO D10, the default sign pin).
- `usebl=1` uses the onboard LED as the output instead of an external pin.
- Persists to NVS and reboots; WiFi credentials are kept so the device
  reconnects on its own.
- Response fields: `ok`, `out_pin`, `usebl`, `rebooting`.
- Errors:
  - `400` if neither a valid `out` (0..48) nor `usebl=1` is provided.

POST /api/led?ledah=0|1
- Sets the onboard LED active level. `1` = active HIGH (ON drives the pin
  HIGH), `0` = active LOW (ON drives the pin LOW). Use `0` for boards whose
  onboard LED lights when the GPIO is LOW (e.g. XIAO ESP32-C6, GPIO15).
- Applies live (no reboot) and persists to NVS.
- Response fields: `ok`, `led_active_high`.
- Errors:
  - `400` if `ledah` is missing.

GET /api/config
- Returns stored configuration.
- Response fields:
  - `ok`
  - `ssid`
  - `hostname`
  - `out`
  - `ledah`
  - `output_mode`
  - `br_period_ms`
  - `br_min_pct`
  - `br_max_pct`

Reboot / factory reset
----------------------

POST /api/reboot
- Soft reboot — no state cleared. Auth required.
- Response fields: `ok`, `rebooting`.
- Reboot is scheduled asynchronously so the response flushes before the
  chip restarts.

POST /api/factory-reset[?deep=1]
- Clears the `cfg` Preferences namespace (Wi-Fi creds, auth, AWS config,
  output settings) and reboots. Same effect as a 5 s BOOT hold.
- With `?deep=1`, also clears the `mfg` namespace — rotates the setup-AP
  password and invalidates any printed sticker. Same effect as a 15 s
  BOOT hold or `RESET-ALL` over Serial.
- Auth required.
- Response fields: `ok`, `deep`, `rebooting`.

AWS IoT
-------

Available when `ENABLE_AWS_IOT=1` at build time (the default). All
endpoints require auth. See README.md → "AWS IoT Core (optional)" for
the broader context.

POST /api/aws/provision
- Writes endpoint, cert, key, optional root CA, and optional Thing name
  to the `aws_iot` Preferences namespace and re-initializes the MQTT
  module.
- Body (JSON): `{endpoint, root_ca?, thing_name?, cert, key}`.
- `root_ca` empty → bundled Amazon Root CA 1 is used at runtime.
- `thing_name` empty → falls back to `onair-<mac12>`.
- Response fields: `ok`, `thing` (resolved name), `endpoint`.
- Errors:
  - `{ok: false, error: "Bad JSON"}` on parse failure.
  - `{ok: false, error: "endpoint, cert, and key are required"}` if any
    required field is empty.

GET /api/aws/status
- Returns module status. Never echoes PEM material.
- Response fields:
  - `enabled` (bool — always true when compiled in)
  - `provisioned` (bool — required NVS keys are present)
  - `connected` (bool — MQTT link is up)
  - `last_rc` (int — last PubSubClient `state()`; 0 = never tried)
  - `thing` (string — resolved Thing name; empty when unprovisioned)
  - `endpoint` (string — empty when unprovisioned)

POST /api/aws/forget
- Clears the `aws_iot` Preferences namespace and disconnects the MQTT
  link. Other namespaces (cfg, mfg) are untouched.
- Response fields: `ok`.

POST /api/aws/claim
- Runs Fleet Provisioning by Claim synchronously. Blocks for ~10–30 s
  (15 s per MQTT phase under timeout).
- Body (JSON): `{endpoint, root_ca?, claim_cert, claim_key,
  template_name, parameters?}`. `parameters` is the JSON object merged
  into the `RegisterThing` payload; if omitted, defaults to
  `{"SerialNumber": "<mac12>"}`.
- On success, the resulting per-device identity is persisted via the
  same path as `/api/aws/provision`; the claim cert is dropped from
  memory.
- Response fields: `ok`, `status` (`awsClaimResultName(result)`),
  `thing` (assigned Thing name), `error` (on failure).

OTA
---

GET /update
- HTML upload form (requires auth, only in STA mode).

POST /update
- Upload a compiled `.bin` as multipart form data, field name `firmware`.
- On success: 303 redirect to `/update/done`. The browser navigates via
  GET, so back/refresh is safe (no "Confirm form resubmission"
  warning). The target page polls `/api/status` every 1 s for up to
  60 s and navigates to `/` once the device is back from the restart.
- With `Accept: application/json`: returns `{ok, rebooting}` directly
  with HTTP 200; HTTP 500 with `{ok: false, error: "Update failed"}`
  on failure.
- On HTML failure: inline error page, no redirect, no restart (retry
  immediately).

GET /update/done
- The post-upload waiting page that the 303 redirect lands on. Auth
  required. Polls `/api/status` until the device responds, then
  redirects to `/`.
