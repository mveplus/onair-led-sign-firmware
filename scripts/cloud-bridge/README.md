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
