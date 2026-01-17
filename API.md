# Portal API Contract

This document describes the HTTP API exposed by `Portal_API_OTA.ino`.
All endpoints return JSON unless otherwise noted.

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

OTA
---

GET /update
- HTML upload form (requires auth, only in STA mode).

POST /update
- Upload a compiled `.bin`.
- Returns JSON with `ok` and `rebooting` on success.
