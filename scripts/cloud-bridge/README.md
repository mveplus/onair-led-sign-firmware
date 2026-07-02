# Cloud bridge

A tiny AWS Lambda + API Gateway HTTP endpoint that lets the Chrome
extension flip the OnAir sign **from anywhere on the internet** — by
publishing to the device's AWS IoT MQTT topic instead of hitting the
device's local HTTP API.

```
[Chrome extension]
       │
       │  POST /onair  +  Authorization: Bearer <token>
       ▼
[API Gateway HTTP API]
       │  Lambda integration
       ▼
[Lambda: onair-publish]    — auths the bearer, checks allowlist
       │
       │  iot:Publish onair/<thing>/cmd  {"mode": 0|1|2}
       ▼
[AWS IoT Core]
       │  MQTT (persistent device subscription via aws_iot.cpp)
       ▼
[OnAir device]             — setOutputMode(mode)
```

## Quick start

Requires the device to already be provisioned via
`scripts/provision-device.sh` (or any of the
[PROVISIONING_PLAN.md](../../PROVISIONING_PLAN.md) Phase 1 paths) so the
device is subscribed to `onair/<thing>/cmd`.

```bash
AWS_PROFILE=onair-iot \
ALLOWED_THINGS=onair-test-1,onair-office \
  ./scripts/cloud-bridge/deploy.sh
```

The script is idempotent — re-run it after editing `lambda_function.py`
to ship a new version. The bearer token is cached locally at
`.onair-bridge-token` so subsequent runs don't rotate it. Delete that
file (and re-deploy) if you need a fresh token.

At the end you'll get:

- An **API endpoint** like
  `https://abc123.execute-api.eu-west-1.amazonaws.com`
- A **bearer token** stashed at `.onair-bridge-token` (chmod 600)
- A copy-pasteable smoke test (off → on → breathing → off)

## Reading device state (cloud `verify`)

The same endpoint answers a **`GET ?thing=<thing>`** that returns the
device's last reported state, read from its AWS IoT **Device Shadow**:

```json
{ "ok": true, "thing": "onair-test-1", "mode": 2,
  "reported": { "mode": 2, "output_mode": "breathing", "rssi": -55 }, "ts": 1700000000 }
```

The firmware reports into the shadow on every `setOutputMode` (see
`aws_iot.cpp`), so this is the durable last-known state the extension's
`verify` reconcile compares against when the LAN path is unreachable.
`POST` still publishes a command; only the HTTP method differs.

The device cert's `OnAirSignPolicy` already allows the shadow topics, and
`deploy.sh` grants the Lambda `iot:GetThingShadow` (re-run it to upgrade an
existing role). No new API Gateway route is needed — the `$default` route
forwards both methods.

### Validate the whole path without the device

`validate-shadow.sh` seeds a shadow (as if the firmware reported it),
reads it straight from AWS, then reads it back through the Lambda:

```bash
AWS_PROFILE=onair-iot ./scripts/cloud-bridge/validate-shadow.sh onair-test-1 2
# → PASS — Lambda returned mode=2 from the shadow.
```

Unit tests for the Lambda run offline (no AWS): `python3 -m pytest` in this
directory.

## Chrome extension wiring

Add three settings the user fills in once:

| Key | Value |
|---|---|
| `cloudBase` | the API endpoint URL |
| `cloudToken` | contents of `.onair-bridge-token` |
| `thing`     | e.g. `onair-test-1` |

The MV3 manifest's `host_permissions` should include the endpoint:

```jsonc
{
  "host_permissions": [
    "http://*/api/*",
    "https://*.execute-api.eu-west-1.amazonaws.com/*"
  ]
}
```

Then a single helper picks local-first, cloud-fallback:

```js
async function setOnAir(mode /* 0|1|2 */) {
  return (await tryLocal(mode)) || tryCloud(mode);
}
```

See the device's `README.md → AWS IoT Core` for the topic shape on the
firmware side.

## Testing & troubleshooting

Smoke-test the endpoint with `curl` and read the HTTP status — each layer
fails with a distinct code, so the response tells you where the chain
broke:

```bash
TOKEN=$(cat .onair-bridge-token)
URL=https://<api-id>.execute-api.eu-west-1.amazonaws.com/onair
curl -sS -w '  [%{http_code}]\n' -X POST "$URL" \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"thing":"onair-office","mode":1}'
```

| Status | Meaning | Fix |
|---|---|---|
| `401` | bad / missing bearer token | use the value in `.onair-bridge-token` |
| `400` | missing/invalid `thing` or `mode` | `mode` must be `0` (off), `1` (on), `2` (breathing) |
| `403` | `thing` not in `ALLOWED_THINGS` | re-run `deploy.sh` with your real Thing name in `ALLOWED_THINGS` |
| `200 {"ok":true}` | published to `onair/<thing>/cmd` | if the sign still doesn't move, the issue is device-side — see below |

A **`200` is not proof the sign changed** — it only means the Lambda
published. The `thing` must name a device actually subscribed to
`onair/<thing>/cmd` (the firmware logs `subscribed onair/<thing>/cmd` on
connect). To isolate the device from the bridge, publish straight to the
topic and watch the device serial:

```bash
aws iot-data publish --topic 'onair/onair-office/cmd' \
  --cli-binary-format raw-in-base64-out --payload '{"mode":1}'
```

If that flips the sign but the `curl` doesn't, the problem is the bridge
(token/allowlist); if neither does, it's device-side (MQTT subscription).

## Tearing down

The script doesn't delete anything (intentional — re-running won't
accidentally drop your token or endpoint). If you want to remove the
deployment, run by hand:

```bash
AWS_PROFILE=onair-iot aws apigatewayv2 delete-api \
  --api-id "$(aws apigatewayv2 get-apis --query "Items[?Name=='onair-publish-api'].ApiId|[0]" --output text)"

AWS_PROFILE=onair-iot aws lambda delete-function --function-name onair-publish

AWS_PROFILE=onair-iot aws iam delete-role-policy --role-name onair-publish-role --policy-name iot-publish-onair
AWS_PROFILE=onair-iot aws iam detach-role-policy --role-name onair-publish-role \
  --policy-arn arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole
AWS_PROFILE=onair-iot aws iam delete-role --role-name onair-publish-role
```

## Security notes

- `SHARED_TOKEN` is a bearer token: anyone holding it can flip the sign.
  Fine for a single-user extension; for multi-user, swap the Lambda's
  auth for Cognito + per-user JWTs and check `cognito:sub` against an
  allowlist.
- `ALLOWED_THINGS` is enforced inside the Lambda (set as an env var on
  the function). Bumping it doesn't require touching API Gateway or
  IAM.
- The IAM role attached to the Lambda is scoped to
  `iot:Publish onair/*/cmd` — nothing else. Even if the function is
  compromised, the blast radius is "flip OnAir signs", not "anything in
  the AWS account".
- `.onair-bridge-token` is added to `.gitignore` by this PR so it
  doesn't accidentally ride along with a commit.
