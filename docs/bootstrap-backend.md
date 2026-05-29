# Custom Bootstrap Backend — Contract

This document defines the HTTP contract a third-party "bootstrap backend"
can implement so that a stock onair-led-sign-firmware device, flashed
from a public release, can be provisioned against any AWS IoT account
without using the maintainer's claim certificate.

This is **Phase 4** of `PROVISIONING_PLAN.md`. The contract is published
in v1 so others can prototype against it; the captive-portal UI does
not yet expose a "Backend URL" option. When demand materializes the
on-device wiring is small (one new captive-portal section + one HTTP
request — the contract is unchanged from below).

> Two simpler provisioning paths ship in v1:
> 1. **Paste credentials** — owner generates cert/key with the AWS CLI
>    and pastes into the captive portal. Zero AWS-side setup.
> 2. **Fleet Provisioning by Claim** — owner brings a claim cert; device
>    auto-bootstraps via AWS IoT's provisioning topics.
>
> The backend path below adds a third option: the owner hosts a small
> service that returns per-device credentials on demand. Useful when:
> - The owner wants to issue device credentials from infrastructure
>   they already run rather than maintain a claim cert.
> - The same backend serves several firmware families (this one, a
>   sensor firmware, etc.).
> - Per-device authorization needs custom logic (MAC allowlists,
>   per-customer policies, expiry).

---

## On-device flow

When the owner configures a Backend URL in the captive portal:

```
device → backend                 backend → device
─────────────────                ─────────────────
POST <backend_url>               200 OK
Content-Type: application/json   Content-Type: application/json
Authorization: Bearer <token>?   (see "Response" below)
{ "mac": ..., ... }
```

The device writes the response straight into the same NVS record used
by `POST /api/aws/provision` (Phase 1) and immediately re-inits the
persistent MQTT module. The device does **not** retry on its own — a
failed call surfaces in the captive portal so the owner can iterate.

## Request

```http
POST {{backend_url}} HTTP/1.1
Content-Type: application/json
Authorization: Bearer {{bearer_token}}   ← optional, captive-portal field
```

Body:

```json
{
  "mac": "AA:BB:CC:DD:EE:FF",
  "chip_id": "esp32c6-aabbccddeeff",
  "fw_version": "2026-05-29+9a8b7c6"
}
```

| Field         | Type   | Notes                                         |
|---------------|--------|-----------------------------------------------|
| `mac`         | string | Base STA MAC, colon-separated, uppercase hex. |
| `chip_id`     | string | Stable per-device identifier; see below.      |
| `fw_version` | string | Build version baked at compile time.          |

`chip_id` is `"esp32c6-" + lowercase MAC12` (no separators). It is
stable across reboots and factory resets.

## Response

On success (`200 OK`):

```json
{
  "endpoint":    "xxxx-ats.iot.<region>.amazonaws.com",
  "root_ca":     "-----BEGIN CERTIFICATE-----\n...",
  "thing_name":  "onair-<assigned-name>",
  "cert":        "-----BEGIN CERTIFICATE-----\n...",
  "key":         "-----BEGIN PRIVATE KEY-----\n..."
}
```

| Field        | Type   | Required | Notes                                                       |
|--------------|--------|----------|-------------------------------------------------------------|
| `endpoint`   | string | yes      | AWS IoT account ATS endpoint, no scheme, no port.           |
| `root_ca`    | string | no       | PEM. Omit/empty → bundled Amazon Root CA 1 is used.         |
| `thing_name` | string | no       | Omit/empty → device defaults to `onair-<mac12>` at runtime. |
| `cert`       | string | yes      | PEM, per-device client certificate.                         |
| `key`        | string | yes      | PEM, per-device client private key.                         |

On failure, return any non-`2xx` status. The device surfaces the
response body verbatim in the captive portal (truncated to 256 bytes).

## Authentication

The captive portal accepts an optional bearer token alongside the URL.
If set, the device sends `Authorization: Bearer <token>` with every
request. The contract does not prescribe additional schemes; HMAC
signing, mTLS, or short-lived JWT auth can be layered on top by the
backend operator.

Recommended minimum: a long random bearer token, scoped to a single
fleet. The token sits in the device's NVS once configured and stays
there — assume it lives as long as the device.

## Security considerations

- **TLS is mandatory.** The device validates the server certificate
  against the system trust store baked at compile time. Self-signed
  backends require the owner to also paste an override Root CA in the
  captive portal.
- **Backend MUST authenticate the request.** The bearer token alone is
  weak (a stolen device exposes it). For high-trust deployments, layer
  on a MAC allowlist, a per-fleet HMAC signature, or short-lived
  signed JWTs the captive portal mints on demand.
- **Backend MUST audit issued credentials.** A single per-device cert
  can be revoked through AWS IoT's `UpdateCertificate` API; track the
  `certificateId` returned by `CreateKeysAndCertificate` alongside the
  MAC.
- **Rate limiting.** A leaked bearer token shouldn't be able to mint
  unlimited certificates. Apply per-token and per-MAC limits at the
  edge.

## Reference Lambda implementation (sketch)

The example below shows the rough shape of an API Gateway → Lambda
integration that issues per-device certs against the caller's own AWS
account. Adapt to your IAM / auth posture.

```python
# bootstrap_backend.py — minimal example, not production-ready.
import json
import os
import boto3

iot = boto3.client("iot")

ALLOWED_MACS = set(os.environ["ALLOWED_MACS"].split(","))   # MAC allowlist
BEARER_TOKEN = os.environ["BOOTSTRAP_TOKEN"]
THING_POLICY = os.environ["THING_POLICY"]                   # IoT policy name
ENDPOINT     = os.environ["AWS_IOT_ENDPOINT"]               # ATS host
THING_GROUP  = os.environ.get("THING_GROUP")                # optional

def handler(event, _ctx):
    headers = {k.lower(): v for k, v in (event.get("headers") or {}).items()}
    if headers.get("authorization") != f"Bearer {BEARER_TOKEN}":
        return _resp(401, {"error": "unauthorized"})

    body = json.loads(event.get("body") or "{}")
    mac = (body.get("mac") or "").upper()
    if mac not in ALLOWED_MACS:
        return _resp(403, {"error": "device not on allowlist"})

    # Mint a fresh cert. AWS returns cert + key once at creation time.
    cert = iot.create_keys_and_certificate(setAsActive=True)
    cert_arn = cert["certificateArn"]
    thing_name = f"onair-{mac.replace(':', '').lower()}"

    iot.create_thing(thingName=thing_name)
    if THING_GROUP:
        iot.add_thing_to_thing_group(
            thingGroupName=THING_GROUP, thingName=thing_name,
        )
    iot.attach_thing_principal(thingName=thing_name, principal=cert_arn)
    iot.attach_policy(policyName=THING_POLICY, target=cert_arn)

    return _resp(200, {
        "endpoint":   ENDPOINT,
        "thing_name": thing_name,
        "cert":       cert["certificatePem"],
        "key":        cert["keyPair"]["PrivateKey"],
        # root_ca omitted — device falls back to bundled Amazon Root CA 1
    })


def _resp(code, body):
    return {
        "statusCode": code,
        "headers": {"Content-Type": "application/json"},
        "body": json.dumps(body),
    }
```

Deploy via SAM/CDK/Terraform; protect with API Gateway throttling and
a CloudWatch alarm on `4XXError`. Capture the `certificateArn` and
`thingName` in a DynamoDB table so revocation has a primary key to
work from.

## Testing the contract

The device-side wiring isn't shipped yet (Phase 4 is doc-only in v1).
For now, the contract can be exercised directly against
`POST /api/aws/provision` — the captive-portal Backend URL flow would
just be a thin wrapper that does the HTTP call and forwards the
response into provision. Example:

```bash
curl -fsS -u admin:esp32c6 \
  -X POST http://<device-ip>/api/aws/provision \
  -H 'Content-Type: application/json' \
  --data-binary @<(curl -fsS \
       -X POST https://your-backend.example.com/bootstrap \
       -H "Authorization: Bearer $BOOTSTRAP_TOKEN" \
       -H 'Content-Type: application/json' \
       -d '{"mac":"AA:BB:CC:DD:EE:FF","chip_id":"esp32c6-aabbccddeeff","fw_version":"dev"}')
```

A successful round-trip provisions the device with whatever cert/key
the backend returned. The same backend can serve the eventual Backend
URL captive-portal flow without changes.
