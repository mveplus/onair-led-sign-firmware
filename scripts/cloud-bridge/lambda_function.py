"""OnAir cloud bridge — Lambda backend for the Chrome extension.

Trigger: HTTP POST from API Gateway with JSON body
    {"thing": "onair-test-1", "mode": 0|1|2}
where mode is 0=off, 1=on, 2=breathing.

Auth: a shared bearer token in the `Authorization` header. The token
is set as the SHARED_TOKEN environment variable on the Lambda; the
deploy script stashes it locally in `.onair-bridge-token` so the
extension and CI can read it back.

On success the function publishes {"mode": N} to the AWS IoT topic
`onair/<thing>/cmd`, which the firmware module in aws_iot.cpp
subscribes to and dispatches via setOutputMode().
"""

import json
import os

import boto3

REGION = os.environ.get("AWS_REGION", "eu-west-1")
THING_ALLOWLIST = {t.strip() for t in os.environ.get("ALLOWED_THINGS", "").split(",") if t.strip()}
SHARED_TOKEN = os.environ["SHARED_TOKEN"]

iot = boto3.client("iot-data", region_name=REGION)


def lambda_handler(event, _context):
    method = (event.get("requestContext") or {}).get("http", {}).get("method", "POST").upper()
    if method == "OPTIONS":
        return _resp(204, body=None)

    headers = {k.lower(): v for k, v in (event.get("headers") or {}).items()}
    if headers.get("authorization") != f"Bearer {SHARED_TOKEN}":
        return _resp(401, {"error": "unauthorized"})

    try:
        body = json.loads(event.get("body") or "{}")
    except json.JSONDecodeError:
        return _resp(400, {"error": "invalid JSON"})

    thing = body.get("thing")
    mode = body.get("mode")
    if not thing:
        return _resp(400, {"error": "missing 'thing'"})
    if THING_ALLOWLIST and thing not in THING_ALLOWLIST:
        return _resp(403, {"error": f"thing '{thing}' not in ALLOWED_THINGS"})
    if mode not in (0, 1, 2):
        return _resp(400, {"error": "mode must be 0 (off), 1 (on), or 2 (breathing)"})

    iot.publish(
        topic=f"onair/{thing}/cmd",
        qos=0,
        payload=json.dumps({"mode": mode}),
    )
    return _resp(200, {"ok": True, "thing": thing, "mode": mode})


def _resp(code, body):
    headers = {
        "Access-Control-Allow-Origin": "*",
        "Access-Control-Allow-Headers": "Authorization, Content-Type",
        "Access-Control-Allow-Methods": "POST, OPTIONS",
    }
    if body is None:
        return {"statusCode": code, "headers": headers}
    headers["Content-Type"] = "application/json"
    return {"statusCode": code, "headers": headers, "body": json.dumps(body)}
