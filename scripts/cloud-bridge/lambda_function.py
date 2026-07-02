"""OnAir cloud bridge — Lambda backend for the Chrome extension.

Two operations, dispatched by HTTP method on the same API Gateway route:

  POST  {"thing": "onair-test-1", "mode": 0|1|2}   (or ?thing=…&mode=…)
        Publishes {"mode": N} to the AWS IoT topic onair/<thing>/cmd,
        which the firmware (aws_iot.cpp) dispatches via setOutputMode().

  GET   ?thing=onair-test-1
        Reads the device's last reported state from its AWS IoT Device
        Shadow and returns {"thing", "mode", "reported", "ts"}. This is
        what the extension's cloud `verify` reconcile uses to detect
        drift when the LAN path is unreachable. The firmware reports into
        the classic shadow on every setOutputMode.

Auth: a shared bearer token in the `Authorization` header (SHARED_TOKEN
env var). The deploy script stashes it locally in `.onair-bridge-token`.
"""

import json
import os

import boto3
from botocore.exceptions import ClientError

REGION = os.environ.get("AWS_REGION", "eu-west-1")
THING_ALLOWLIST = {t.strip() for t in os.environ.get("ALLOWED_THINGS", "").split(",") if t.strip()}
SHARED_TOKEN = os.environ["SHARED_TOKEN"]

iot = boto3.client("iot-data", region_name=REGION)


def parse_reported_mode(shadow_doc):
    """Extract the device mode (0|1|2) from a Device Shadow document, or
    None when it isn't determinable. Prefers an explicit `mode`, then the
    `output_mode` string, then a legacy `state` boolean — mirroring the
    firmware /api/status body and the extension's parseDeviceMode so all
    three agree on the mapping. Pure/side-effect-free for unit testing."""
    reported = ((shadow_doc or {}).get("state") or {}).get("reported") or {}

    if reported.get("mode") is not None:
        try:
            m = int(reported["mode"])
        except (TypeError, ValueError):
            return None
        return m if m in (0, 1, 2) else None

    om = str(reported.get("output_mode", "")).lower()
    if om == "off":
        return 0
    if om == "on":
        return 1
    if om == "breathing":
        return 2

    st = reported.get("state")
    if isinstance(st, bool):
        return 1 if st else 0
    return None


def _thing_from(event):
    query = event.get("queryStringParameters") or {}
    try:
        body = json.loads(event.get("body") or "{}")
    except json.JSONDecodeError:
        body = {}
    thing = body.get("thing") or query.get("thing")
    mode_raw = body["mode"] if "mode" in body else query.get("mode")
    return thing, mode_raw


def handle_get(event):
    """Read the device's last reported state from its shadow."""
    thing, _ = _thing_from(event)
    if not thing:
        return _resp(400, {"error": "missing 'thing' (?thing=…)"})
    if THING_ALLOWLIST and thing not in THING_ALLOWLIST:
        return _resp(403, {"error": f"thing '{thing}' not in ALLOWED_THINGS"})

    try:
        resp = iot.get_thing_shadow(thingName=thing)
    except ClientError as e:
        code = e.response.get("Error", {}).get("Code", "")
        if code == "ResourceNotFoundException":
            return _resp(404, {"error": f"no shadow for '{thing}' yet"})
        raise

    doc = json.loads(resp["payload"].read())
    mode = parse_reported_mode(doc)
    if mode is None:
        return _resp(404, {"error": f"shadow for '{thing}' has no reported mode"})

    reported = ((doc.get("state") or {}).get("reported")) or {}
    return _resp(200, {"ok": True, "thing": thing, "mode": mode,
                       "reported": reported, "ts": doc.get("timestamp")})


def handle_post(event):
    """Publish a command to the device's cmd topic."""
    thing, mode_raw = _thing_from(event)
    if not thing:
        return _resp(400, {"error": "missing 'thing' (body or ?thing=…)"})
    if THING_ALLOWLIST and thing not in THING_ALLOWLIST:
        return _resp(403, {"error": f"thing '{thing}' not in ALLOWED_THINGS"})
    if mode_raw is None:
        return _resp(400, {"error": "missing 'mode' (body or ?mode=0|1|2)"})
    try:
        mode = int(mode_raw)
    except (TypeError, ValueError):
        return _resp(400, {"error": "mode must be 0 (off), 1 (on), or 2 (breathing)"})
    if mode not in (0, 1, 2):
        return _resp(400, {"error": "mode must be 0 (off), 1 (on), or 2 (breathing)"})

    iot.publish(topic=f"onair/{thing}/cmd", qos=0, payload=json.dumps({"mode": mode}))
    return _resp(200, {"ok": True, "thing": thing, "mode": mode})


def lambda_handler(event, _context):
    method = (event.get("requestContext") or {}).get("http", {}).get("method", "POST").upper()
    if method == "OPTIONS":
        return _resp(204, body=None)

    headers = {k.lower(): v for k, v in (event.get("headers") or {}).items()}
    if headers.get("authorization") != f"Bearer {SHARED_TOKEN}":
        return _resp(401, {"error": "unauthorized"})

    if method == "GET":
        return handle_get(event)
    return handle_post(event)


def _resp(code, body):
    headers = {
        "Access-Control-Allow-Origin": "*",
        "Access-Control-Allow-Headers": "Authorization, Content-Type",
        "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
    }
    if body is None:
        return {"statusCode": code, "headers": headers}
    headers["Content-Type"] = "application/json"
    return {"statusCode": code, "headers": headers, "body": json.dumps(body)}
