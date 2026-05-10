# AWS IoT Core Integration — Implementation Plan

Status: **proposed scaffold**, not yet wired into the sketch.

This document is the engineering plan for adding AWS IoT Core support to the
`onair-led-sign-firmware` ESP32-C6 firmware. The existing local HTTP API,
WebSocket, captive portal, OTA endpoint, and web UI **must remain working
untouched** — AWS IoT is an additive transport layer behind a compile-time
feature flag.

---

## 1. Goals

1. Connect the device to AWS IoT Core over MQTT/TLS 1.2 (port 8883) using a
   per-device X.509 client certificate.
2. Allow remote control of the sign (off / on / breathing + parameters)
   through MQTT topics, in parallel with the existing local HTTP and
   WebSocket interfaces.
3. Reflect device state into a **classic Thing Shadow** (`$aws/things/<thing>/shadow/...`)
   so dashboards, Home Assistant, and Lambda consumers see authoritative
   reported state and can request changes via `desired`.
4. Receive OTA firmware updates through **AWS IoT Jobs** instead of (or in
   addition to) the manual `/update` HTTP endpoint.
5. Provision new devices into a fleet automatically using **AWS IoT Fleet
   Provisioning by Claim**, so an unprovisioned ESP32-C6 with only a bootstrap
   certificate becomes a real Thing without any per-device manual steps.
6. Build, version, sign, and publish firmware via **GitHub Actions**, then
   roll it out as an IoT Job to a Thing Group.

## 2. Non-goals

- Replacing or modifying the local HTTP API surface in `API.md`.
- Replacing the captive setup portal flow (it gains *new* fields, but the
  existing Wi-Fi setup behavior is unchanged).
- Mandating AWS — the firmware MUST still build and run with
  `ENABLE_AWS_IOT=0` and behave exactly as it does today.

## 3. Architecture overview

```
                ┌────────────────────────────────────────┐
                │  ESP32-C6  (onair-led-sign-firmware)   │
                │                                        │
   USB serial ──┤  WiFi (STA)                            │
                │   ├─ Captive portal (AP+STA)           │
                │   ├─ HTTP API + WebSocket (local LAN)  │ ← unchanged
                │   ├─ OTA /update (local LAN)           │
                │   └─ AWS IoT module (this work) ◄──────┼─── #ifdef ENABLE_AWS_IOT
                │        ├─ WiFiClientSecure (TLS 1.2)   │
                │        ├─ PubSubClient (MQTT 3.1.1)    │
                │        ├─ Shadow client                │
                │        ├─ Jobs client (OTA)            │
                │        └─ Fleet provisioning (claim)   │
                └────────────────────────────────────────┘
                                 │ MQTT/TLS 8883
                                 ▼
                          AWS IoT Core
                                 │
     ┌───────────────────────────┼───────────────────────────────┐
     ▼                           ▼                               ▼
  Thing Shadow             Rules Engine               IoT Jobs (OTA)
  ($aws/things/...)        → Lambda / SNS / DDB       → S3 firmware bin
```

### Library choice

| Concern         | Library                       | Rationale |
|-----------------|-------------------------------|-----------|
| TLS transport   | `WiFiClientSecure` (Arduino-ESP32 built-in) | mbedTLS, supports root CA + client cert/key, no extra deps |
| MQTT            | **PubSubClient** (Nick O'Leary) | Smallest stable Arduino MQTT client; sufficient for QoS 0/1; easier to audit than AWS IoT SDK on Arduino |
| JSON            | `ArduinoJson` 7.4.2 (already in project) | Reuse; shadow + jobs payloads are small |
| OTA write       | `Update` (already in project) | Reuse the same flash logic the `/update` route uses |

> Trade-off: PubSubClient does not implement MQTT 5.0 or persistent sessions
> well. For this device that is fine — the shadow handles "what was the last
> state" and Jobs has its own retry semantics. If we later need MQTT 5 or
> larger payloads we can swap in `espMqttClient` without touching the
> public surface of `aws_iot.h`.

### Topic plan

Thing name = MQTT client ID = `onair-<MAC12>` (lowercase hex, e.g. `onair-a1b2c3d4e5f6`).

Application topics (under our own namespace):

| Direction        | Topic                              | QoS | Payload |
|------------------|------------------------------------|-----|---------|
| sub (cloud→dev)  | `onair/<thing>/cmd`                | 1   | `{"mode":"on"\|"off"\|"breathing","period_ms":1500,"min":40,"max":1023}` |
| pub (dev→cloud)  | `onair/<thing>/state`              | 0   | full status JSON, same shape as `GET /api/status` |
| pub (dev→cloud)  | `onair/<thing>/event/boot`         | 0   | `{"fw":"<FW_VERSION>","reason":"<reset_reason>"}` |
| pub (dev→cloud)  | `onair/<thing>/event/error`        | 0   | `{"code":"...", "msg":"..."}` |

Reserved AWS topics (we use them, we do not own them):

| Purpose                    | Topic |
|----------------------------|-------|
| Shadow update from device  | `$aws/things/<thing>/shadow/update` |
| Shadow update accept/reject| `$aws/things/<thing>/shadow/update/accepted` `/rejected` |
| Shadow delta               | `$aws/things/<thing>/shadow/update/delta` |
| Jobs notify-next           | `$aws/things/<thing>/jobs/notify-next` |
| Jobs get pending           | `$aws/things/<thing>/jobs/get` |
| Jobs update execution      | `$aws/things/<thing>/jobs/<jobId>/update` |
| Fleet provisioning (claim) | `$aws/certificates/create/json` and `$aws/provisioning-templates/<tpl>/provision/json` |

## 4. IAM / IoT policy

`OnAirSignPolicy` — least-privilege, parameterized by `${iot:ClientId}` /
`${iot:Connection.Thing.ThingName}`:

```jsonc
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": "iot:Connect",
      "Resource": "arn:aws:iot:*:*:client/${iot:ClientId}"
    },
    {
      "Effect": "Allow",
      "Action": ["iot:Publish", "iot:Receive"],
      "Resource": [
        "arn:aws:iot:*:*:topic/onair/${iot:Connection.Thing.ThingName}/*",
        "arn:aws:iot:*:*:topic/$aws/things/${iot:Connection.Thing.ThingName}/shadow/*",
        "arn:aws:iot:*:*:topic/$aws/things/${iot:Connection.Thing.ThingName}/jobs/*"
      ]
    },
    {
      "Effect": "Allow",
      "Action": "iot:Subscribe",
      "Resource": [
        "arn:aws:iot:*:*:topicfilter/onair/${iot:Connection.Thing.ThingName}/cmd",
        "arn:aws:iot:*:*:topicfilter/$aws/things/${iot:Connection.Thing.ThingName}/shadow/*",
        "arn:aws:iot:*:*:topicfilter/$aws/things/${iot:Connection.Thing.ThingName}/jobs/*"
      ]
    }
  ]
}
```

A separate `OnAirBootstrapPolicy` is used during fleet provisioning by claim
and only allows the `$aws/certificates/create/json` and
`$aws/provisioning-templates/onair/provision/json` topics.

## 5. Files in this change

| Path | Purpose |
|------|---------|
| `PLAN.md` | This document. |
| `scripts/provision-device.sh` | One-shot AWS CLI script: create Thing, generate cert keypair, attach `OnAirSignPolicy`, write certs to `certs/<thing>/`. |
| `.github/workflows/build.yml` | CI: build on every push; on `v*` tag, upload `.bin` to S3 and create an IoT Job. |
| `src/aws_iot.h` / `src/aws_iot.cpp` | Module scaffold: `connect()`, `loop()`, `publishState()`, `handleCommand()`. |
| `secrets.h` *(gitignored)* | Per-device or per-build constants: endpoint host, root CA, client cert, client key, optional bootstrap cert. |
| `certs/` *(gitignored)* | Output of `provision-device.sh`: `*.cert.pem`, `*.private.key`, `AmazonRootCA1.pem`. |
| `.gitignore` | Excludes the two above. |

## 6. Configuration sources

The AWS IoT module reads config in this order (first hit wins):

1. **`secrets.h`** — compile-time string constants. Useful for development
   and for the early single-Thing flow before fleet provisioning is set up.
2. **NVS / `Preferences`** namespace `aws` — `endpoint`, `thing_name`,
   `root_ca`, `client_cert`, `client_key`. Populated by either:
   - the **captive setup portal** (planned new section: an "AWS IoT" panel
     where the user can paste endpoint + cert/key), or
   - the **fleet provisioning** flow on first boot.

The portal change is intentionally additive: a new collapsed section under
the existing settings, with empty defaults so an existing user upgrading
firmware sees no behavioral change until they fill it in.

## 7. Module surface (`src/aws_iot.h`)

```cpp
namespace AwsIot {
  bool begin();                    // load config, set up TLS client, no network yet
  bool connect();                  // (re)connect MQTT — call when WiFi STA is up
  void loop();                     // pump MQTT, handle reconnect with backoff
  bool publishState(const JsonDocument& doc); // publishes to onair/<thing>/state and shadow reported
  bool isConnected();
  // Internal callbacks (not part of public stub):
  //   handleCommand(topic, payload) — onair/<thing>/cmd
  //   handleShadowDelta(payload)    — $aws/things/<thing>/shadow/update/delta
  //   handleJobNotify(payload)      — $aws/things/<thing>/jobs/notify-next → kick off OTA
}
```

The module is gated by `#define ENABLE_AWS_IOT 1` in `secrets.h` (or the
build flag). When the flag is 0, `aws_iot.cpp` compiles to empty stubs and
adds zero bytes to flash beyond the function symbols.

## 8. Integration points in `onair-led-sign-firmware.ino`

These are the *only* places the sketch needs to be touched. They are listed
here for reviewers; they are **not** applied in this commit because the
module is still a scaffold.

1. After `WiFi.begin(...)` succeeds in STA mode → `AwsIot::begin(); AwsIot::connect();`.
2. In `loop()` → `AwsIot::loop();` (cheap; no-op when disabled).
3. After every state change in `applyOutputMode()` → `AwsIot::publishState(currentStatusJson());`.
4. `AwsIot::handleCommand(...)` calls the same internal `applyOutputMode()`
   that HTTP and WebSocket already call — single code path, no duplication.

## 9. OTA via IoT Jobs

Job document format (matches the AWS IoT Jobs convention):

```json
{
  "operation": "ota",
  "fw_version": "2026-05-10+abcd123",
  "url": "https://onair-fw.s3.eu-west-1.amazonaws.com/firmware/onair-2026-05-10+abcd123.bin",
  "sha256": "<hex>",
  "size": 1234567
}
```

Device flow on `notify-next`:

1. Mark execution `IN_PROGRESS`.
2. HTTPS GET `url` (presigned or public-read), stream into `Update`.
3. Verify SHA-256 against `sha256` before `Update.end()`.
4. On success: mark `SUCCEEDED`, publish boot event with new `FW_VERSION`
   after reboot.
5. On any failure: mark `FAILED` with reason, do **not** reboot.

Rollout strategy is set on the Job, not the device: AWS IoT handles
percentage-based rollouts and abort thresholds.

## 10. Fleet provisioning by claim (future, scaffolded)

Per-device manual provisioning (`scripts/provision-device.sh`) is fine for
the first ~10 boards. For volume, the device ships with a single
**bootstrap claim certificate** baked into firmware, then on first boot:

1. Connect with the claim cert (only allowed to talk to the provisioning
   topics).
2. Publish to `$aws/certificates/create/json` → receive a real per-device
   keypair.
3. Publish to `$aws/provisioning-templates/onair/provision/json` with
   `{"certificateOwnershipToken": "...", "parameters": {"SerialNumber": "<MAC>"}}`.
4. Receive the new ThingName + final cert; persist into NVS; reconnect
   using the per-device cert.
5. Wipe the claim cert from RAM.

This requires a provisioning template + pre-provisioning Lambda hook on the
AWS side. **Out of scope for the first commit** — only the topic constants
and the persistence schema are reserved now so the wire format is stable.

## 11. CI/CD pipeline

Triggered by `.github/workflows/build.yml`:

- **On every push to any branch** → build for the default FQBN
  (`esp32:esp32:XIAO_ESP32C6`) using `scripts/docker-build.sh`. Upload
  `build/onair-led-sign-firmware.ino.bin` and `*.merged.bin` as workflow
  artifacts. This catches breakage early without needing AWS credentials.
- **On tag `v*.*.*`** → in addition:
  - Compute SHA-256 of the `.bin`.
  - `aws s3 cp` the binary to `s3://${FW_BUCKET}/firmware/onair-<tag>.bin`.
  - `aws iot create-job` against Thing Group `OnAirSigns` with the job
    document above and a 10% / 50% / 100% rollout schedule.
  - Required GitHub Secrets: `AWS_ROLE_ARN` (used via OIDC, not long-lived
    keys), `AWS_REGION`, `FW_BUCKET`, `FW_THING_GROUP`.

## 12. Security notes

- Private keys are **never** committed. `.gitignore` covers `certs/` and
  `secrets.h`. The CI workflow does not have access to device private
  keys; it only signs/uploads the firmware.
- TLS verification uses `Amazon Root CA 1` pinned at compile time (or in
  NVS). We do not use `setInsecure()`.
- The HTTP `/update` endpoint stays in the firmware for emergency LAN
  recovery, behind the existing Basic Auth. It is intentionally redundant
  with IoT Jobs.
- Factory reset (BOOT held 5s) clears the `aws` NVS namespace too.

## 13. Open questions (do not block the scaffold)

1. Single shared `OnAirSignPolicy` per fleet, or per-device policies? →
   plan: single policy, scoped by `${iot:Connection.Thing.ThingName}`.
2. Should `state` publishes be QoS 0 or 1? → start with 0; the shadow is
   the authoritative copy.
3. Region: `eu-west-1` is the default in `provision-device.sh` but is
   overridable via `AWS_REGION`.
