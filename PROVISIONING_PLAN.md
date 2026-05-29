# AWS IoT Provisioning & Onboarding Plan

The authoritative plan for AWS IoT integration in this firmware. Zero AWS
credentials in the source tree or in any published binary; provisioning
happens at runtime via three optional paths.

## Goals

1. The public repo and every tagged release contain **zero AWS credentials**
   and **zero binaries** with credentials baked in.
2. The same firmware that ships in tagged releases also supports **opt-in**
   AWS IoT, with all credentials supplied at provisioning time and stored
   only in on-device NVS.
3. Three provisioning paths in parallel, all converging on the same NVS
   record `{endpoint, root_ca, thing_name, cert, key}`:
   - **Paste** — user pastes their own cert/key into the captive portal.
   - **Claim** — Fleet Provisioning by Claim; user supplies a shared claim
     cert and a provisioning template name.
   - **Backend** — user enters a URL of their own bootstrap backend that
     issues per-device credentials.
4. First-time Wi-Fi setup never requires USB/TTY. The SoftAP password is a
   per-device random string generated on first boot and exposed as a
   printable Wi-Fi join QR code (`WIFI:T:WPA;...`).

## Non-goals (for now)

- BLE / Improv-over-BLE provisioning. Revisit if the flash budget allows
  after Phase 5; otherwise ship as a second firmware flavor.
- Matter commissioning.

---

## Phase 0 — Repo hygiene

Pure cleanup. No behavior change.

- [x] `.gitignore` adds `*.pem`, `*.key`, `*.crt`, `build/*.bin`, `*.elf`,
      `*.map`, `build/onair-led-sign-firmware.ino*`.
- [ ] `git rm --cached` the 19 currently-tracked `build/*` artifacts on this
      branch.
- [ ] Move the `#define ENABLE_AWS_IOT 1` out of `aws_iot.cpp:14`. Turn it
      into a build flag set in `scripts/build.sh` /
      `scripts/docker-build.sh`. The .cpp shouldn't override callers.
- [ ] Pick one canonical AWS module path: keep `aws_iot.{h,cpp}` at the
      sketch root (the working implementation); delete `src/aws_iot.{h,cpp}`
      (a stub duplicate).

---

## Phase 1 — Runtime-loaded AWS secrets

The foundation everything else depends on.

- Delete the compile-time `AWS_IOT_ENDPOINT` / `AWS_IOT_ROOT_CA` /
  `AWS_IOT_CLIENT_CERT` / `AWS_IOT_CLIENT_KEY` / `AWS_IOT_THING_NAME`
  macros from `secrets.h`. Replace with a runtime struct populated from the
  ESP32 `Preferences` ("aws_iot" namespace).
- `awsIotSetup()` returns early when any required field is empty — the
  AWS path is dormant on an unprovisioned device.
- `secrets.h` is reduced to a single public default: the bundled
  **Amazon Root CA1** PEM. It is overridable at runtime — the
  `root_ca` field on `POST /api/aws/provision` (and the captive-portal
  AWS section) replaces the bundled CA in NVS when the user provides
  one. Empty/missing in NVS → fall back to the bundled CA1.
- New endpoints:
  - `POST /api/aws/provision` — JSON body
    `{endpoint, root_ca?, thing_name?, cert, key}`; writes to NVS;
    triggers AWS module re-init.
  - `GET /api/aws/status` — returns `{enabled, connected, last_rc,
    thing_name, endpoint}`. Never echoes cert/key bytes.
  - `POST /api/aws/forget` — clears the NVS record.
- Captive portal additions under the existing setup form:
  - Checkbox: "Enable AWS IoT".
  - Radio: "Paste credentials" / "Claim-based" / "Backend URL".
  - Conditional fields rendered per selection (Phase 2 covers Claim,
    Phase 3 covers Backend).

---

## Phase 2 — Per-device AP password + QR sticker

The "Nest Wi-Fi Pro-ish" first-time setup.

### Design

- SSID stays `C6-SETUP-<MAC-last-6>` (no change).
- AP password generated **on first boot**:
  - `salt = esp_random()` (16 bytes from HW RNG), stored once in NVS.
  - `mac = ESP.getEfuseMac()` (48-bit unique per chip).
  - `password = <word>-<word>-<word>-<4-alnum>`, where the three words are
    chosen from the existing wordlist via
    `HMAC-SHA1(salt, mac)` and the four trailing alphanumerics are random.
  - Format example: `meadow-piano-dolphin-x7k2`. Mixed entropy, still
    retypable on a phone keyboard if the QR scan fails.
- After a 5-second BOOT-button factory reset, the salt is regenerated so
  the password rotates.

### Read-out paths (one-time, per device)

1. **Host script `scripts/print-sticker.py`** — runs on the bench right
   after flashing. Opens the device's USB-CDC, issues a debug-only
   `GET-STICKER` Serial command, receives `{mac, ssid, password}`,
   renders a PNG with the QR encoding
   `WIFI:T:WPA;S:<ssid>;P:<password>;;` (camera-app auto-join format).
2. **Captive portal fallback** — the password is also visible on the
   post-login "Setup" page so an owner who is already connected can print
   their own sticker later.
3. **Serial print** kept on first boot for power users.

The `GET-STICKER` Serial command is locked out by an NVS flag after the
first successful STA join — prevents read-out from a stolen, already-
provisioned device.

### Why this shape

- No build-time shared seed → leaking the public binary does not leak any
  device's AP password.
- Host script step is optional. End users without a label printer can
  still scrape the password from the captive portal once connected.
- Words + four chars beats raw random for typability on phone keyboards.

---

## Phase 3 — Fleet Provisioning by Claim

The "tick the checkbox, walk away" UX you asked about. Used by the owner
of the fleet (you) with a shared low-privilege claim cert that bootstraps
each device into its own per-device identity. See AWS docs:
*Provisioning by Claim*.

### Captive-portal inputs

- Endpoint.
- Claim cert + key (paste, **or** an HTTPS URL the device fetches once —
  e.g. a pre-signed S3 link the owner generates).
- Provisioning template name.
- Optional `parameters` JSON merged into `RegisterThing` (e.g. serial,
  room).

### State machine

1. MQTT-connect to endpoint with claim cert.
2. Subscribe `$aws/certificates/create/json/{accepted,rejected}`.
3. Publish `{}` to `$aws/certificates/create/json`.
4. On `accepted`, parse new cert/key + `certificateOwnershipToken`.
5. Subscribe `$aws/provisioning-templates/<tpl>/provision/json/{accepted,rejected}`.
6. Publish `{certificateOwnershipToken, parameters}` to that template
   topic.
7. On `accepted`, persist permanent cert/key + Thing name to NVS, drop
   the claim cert from memory, reconnect with the permanent identity.

Failures surface as human-readable strings in `/api/aws/status` and on
the captive-portal "Setup" page.

### AWS-side one-time setup

- Provisioning template referencing your existing `OnAirSignPolicy` and
  `OnAirSigns` Thing group.
- Claim cert with a policy that allows only `iot:Connect` plus the four
  provisioning topics — nothing else.
- Optional Lambda pre-provisioning hook for MAC allowlisting.

---

## Phase 4 — Custom bootstrap backend (open third-party path)

**v1 status: contract documented only.** The captive-portal radio does
not expose the "Backend URL" option until Phase 4 ships. The contract
below is published so anyone can prototype against it.

### Contract (no backend code in this repo)

- Captive portal "Backend URL" input: backend URL + optional bearer token.
- Device POSTs:

  ```json
  { "mac": "AA:BB:CC:DD:EE:FF",
    "chip_id": "...",
    "fw_version": "..." }
  ```

- Backend responds:

  ```json
  { "endpoint": "...",
    "root_ca": "...",
    "thing_name": "...",
    "cert": "...",
    "key": "..." }
  ```

- Device writes the response straight into the Phase-1 NVS record and
  connects.

A reference Lambda implementation lives in a separate doc
(`docs/bootstrap-backend.md`, TBD) — not part of this repo's build.

---

## Phase 5 — Tagged GitHub releases

`.github/workflows/release.yml` triggered on `v*` tags:

1. Build firmware for `XIAO_ESP32C6` and `dfrobot_beetle_esp32c6` via the
   existing Docker build script. `ENABLE_AWS_IOT=1` is set so the module
   compiles in, but no `secrets.h` is provided — the build only links the
   runtime-NVS loaders.
2. **CI guard**: grep the resulting binaries for `amazonaws`, `BEGIN
   CERTIFICATE`, `BEGIN PRIVATE KEY`, and a few other markers. Fail the
   workflow if any match.
3. Compute SHA256 of each artifact.
4. Upload `*.merged.bin`, `*.bin`, `*.elf`, and a `SHA256SUMS.txt` as
   GitHub Release assets.
5. **Conditional S3 push** — purely env-var-driven, same code path for
   CI and local builds:
   - If `AWS_OTA_BUCKET` is set, `aws s3 cp` the merged binary to
     `s3://$AWS_OTA_BUCKET/firmware/<tag>/onair-led-sign-firmware.merged.bin`.
   - In CI: set as a repo secret or variable; the workflow no-ops when
     absent.
   - Locally: `AWS_OTA_BUCKET=onair-fw ./scripts/build.sh` triggers the
     same push (uses local AWS credentials / `AWS_PROFILE`).
   - No firmware/code change to toggle — bucket presence alone gates it.

The existing on-push workflow keeps producing artifacts for quick
download, but they don't attach to a Release.

---

## Phase 6 (deferred) — BLE / Improv onboarding

Only revisit after Phase 5 lands and we measure remaining flash space.

- **Improv Wi-Fi over BLE** — free web app at `improv-wifi.com`, no
  install. ~400 KB. Chrome/Edge only.
- **ESP BLE Prov** — official Espressif app for iOS + Android. ~500 KB.

If either pushes us past the partition budget, ship a second firmware
flavor (`onair-led-sign-firmware-ble.bin`) selectable at flash time.

---

## File-by-file change summary

| Path | Change |
|---|---|
| `.gitignore` | + `*.pem`, `*.key`, `*.crt`, `build/*.bin`, `*.elf`, `*.map`, `build/onair-led-sign-firmware.ino*` (this session) |
| `aws_iot.cpp` | Drop hardcoded `#define ENABLE_AWS_IOT 1`. Load endpoint/CA/cert/key/thing from `Preferences` at `awsIotSetup()`; return early if missing. |
| `aws_iot.h` | New `struct AwsIotConfig` + `loadFromPrefs()`. |
| `secrets.h` | Reduced to a single public constant: bundled Amazon Root CA1 PEM. Overridable at runtime via `POST /api/aws/provision`. |
| `src/aws_iot.{h,cpp}` | Deleted — consolidate to the sketch-root module. |
| `onair-led-sign-firmware.ino` | Add `/api/aws/*` routes; per-device AP password generation in `setup()`; AWS section in captive portal. |
| `scripts/print-sticker.py` | New host-side tool: read MAC + AP password over USB-CDC, render PNG sticker. |
| `scripts/build.sh` / `docker-build.sh` | Pass `-DENABLE_AWS_IOT=1` build flag explicitly. |
| `.github/workflows/release.yml` | New: tag-triggered release with secret-scan guard. |
| `build/*` | `git rm --cached` everything; new .gitignore keeps them out. |

---

## Decisions (resolved)

1. **Default root CA** — bake Amazon Root CA1 (public). Overridable at
   runtime via the captive portal and `POST /api/aws/provision` so we
   can roll over to CA3/4 or swap to a custom CA without a firmware
   build.
2. **AP password format** — `<word>-<word>-<word>-<4 alphanumeric>` from
   the existing wordlist (extended to ~500 entries). Mixed entropy,
   retypable on a phone keyboard.
3. **`GET-STICKER` lockout** — locks on first successful STA join.
   Factory reset re-arms it (and regenerates the salt → new password).
4. **Custom backend (Phase 4)** — postponed past v1. Captive-portal UI
   does not expose the option; the contract is documented now so others
   can prototype backends. Revisit when there's demand.
5. **S3 OTA push** — env-var-gated (`AWS_OTA_BUCKET`). Same single code
   path runs in CI (as a repo secret/variable) and locally. Bucket
   unset → no push, no S3 dependency.

---

## Suggested order of execution

| # | Phase | Rough effort | Depends on |
|---|---|---|---|
| 1 | Phase 0 — hygiene | ~30 min | — |
| 2 | Phase 1 — runtime secrets | 2–3 h | Phase 0 |
| 3 | Phase 5 — CI release | ~1 h | Phase 1 |
| 4 | Phase 2 — AP password + QR | 2–3 h | independent |
| 5 | Phase 3 — claim flow | 3–4 h | Phase 1 |
| 6 | Phase 4 — backend path | 1–2 h | Phase 1 |
| 7 | Phase 6 — BLE | TBD | after size audit |
