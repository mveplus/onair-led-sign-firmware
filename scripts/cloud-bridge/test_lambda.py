"""Offline unit tests for the cloud-bridge Lambda. No AWS account needed —
the iot-data client is replaced with a fake. Run: python3 -m pytest."""

import io
import json
import os

os.environ.setdefault("SHARED_TOKEN", "secrettoken")
os.environ.setdefault("AWS_REGION", "eu-west-1")

import pytest
from botocore.exceptions import ClientError

import lambda_function as lf

TOKEN = os.environ["SHARED_TOKEN"]


class FakeIot:
    def __init__(self, shadow=None, raise_code=None):
        self.shadow = shadow
        self.raise_code = raise_code
        self.published = []

    def get_thing_shadow(self, thingName):
        if self.raise_code:
            raise ClientError({"Error": {"Code": self.raise_code}}, "GetThingShadow")
        return {"payload": io.BytesIO(json.dumps(self.shadow).encode())}

    def publish(self, topic, qos, payload):
        self.published.append({"topic": topic, "qos": qos, "payload": payload})


def evt(method="GET", thing=None, mode=None, token=TOKEN, body=None):
    q = {}
    if thing is not None:
        q["thing"] = thing
    if mode is not None:
        q["mode"] = str(mode)
    return {
        "requestContext": {"http": {"method": method}},
        "headers": {"authorization": f"Bearer {token}"} if token else {},
        "queryStringParameters": q or None,
        "body": body,
    }


def body_of(resp):
    return json.loads(resp["body"]) if resp.get("body") else None


@pytest.fixture(autouse=True)
def reset_allowlist(monkeypatch):
    # Default: no allowlist (accept any thing) unless a test sets one.
    monkeypatch.setattr(lf, "THING_ALLOWLIST", set())


# ---- parse_reported_mode (pure) ------------------------------------------

@pytest.mark.parametrize("doc,expected", [
    ({"state": {"reported": {"mode": 0}}}, 0),
    ({"state": {"reported": {"mode": 2}}}, 2),
    ({"state": {"reported": {"mode": "1"}}}, 1),          # numeric string
    ({"state": {"reported": {"output_mode": "breathing"}}}, 2),
    ({"state": {"reported": {"output_mode": "OFF"}}}, 0),  # case-insensitive
    ({"state": {"reported": {"state": True}}}, 1),         # legacy bool
    ({"state": {"reported": {"state": False}}}, 0),
    ({"state": {"reported": {"mode": 9}}}, None),          # out of range
    ({"state": {"reported": {}}}, None),
    ({}, None),
    (None, None),
])
def test_parse_reported_mode(doc, expected):
    assert lf.parse_reported_mode(doc) == expected


# ---- GET (shadow read) ---------------------------------------------------

def test_get_returns_reported_mode(monkeypatch):
    monkeypatch.setattr(lf, "iot", FakeIot(shadow={
        "state": {"reported": {"mode": 2, "rssi": -55}}, "timestamp": 1700000000
    }))
    resp = lf.lambda_handler(evt("GET", thing="onair-test-1"), None)
    assert resp["statusCode"] == 200
    b = body_of(resp)
    assert b["thing"] == "onair-test-1"
    assert b["mode"] == 2
    assert b["reported"]["rssi"] == -55  # extra fields pass through (future sensors)


def test_get_missing_thing_is_400(monkeypatch):
    monkeypatch.setattr(lf, "iot", FakeIot())
    resp = lf.lambda_handler(evt("GET"), None)
    assert resp["statusCode"] == 400


def test_get_no_shadow_is_404(monkeypatch):
    monkeypatch.setattr(lf, "iot", FakeIot(raise_code="ResourceNotFoundException"))
    resp = lf.lambda_handler(evt("GET", thing="onair-test-1"), None)
    assert resp["statusCode"] == 404


def test_get_shadow_without_mode_is_404(monkeypatch):
    monkeypatch.setattr(lf, "iot", FakeIot(shadow={"state": {"reported": {"rssi": -55}}}))
    resp = lf.lambda_handler(evt("GET", thing="onair-test-1"), None)
    assert resp["statusCode"] == 404


def test_get_allowlist_reject_is_403(monkeypatch):
    monkeypatch.setattr(lf, "THING_ALLOWLIST", {"onair-allowed"})
    monkeypatch.setattr(lf, "iot", FakeIot(shadow={"state": {"reported": {"mode": 1}}}))
    resp = lf.lambda_handler(evt("GET", thing="onair-evil"), None)
    assert resp["statusCode"] == 403


def test_unexpected_client_error_propagates(monkeypatch):
    monkeypatch.setattr(lf, "iot", FakeIot(raise_code="ThrottlingException"))
    with pytest.raises(ClientError):
        lf.lambda_handler(evt("GET", thing="onair-test-1"), None)


# ---- auth / method -------------------------------------------------------

def test_bad_token_is_401(monkeypatch):
    monkeypatch.setattr(lf, "iot", FakeIot())
    resp = lf.lambda_handler(evt("GET", thing="onair-test-1", token="wrong"), None)
    assert resp["statusCode"] == 401


def test_options_is_204():
    resp = lf.lambda_handler(evt("OPTIONS"), None)
    assert resp["statusCode"] == 204


# ---- POST (command publish, unchanged behavior) --------------------------

def test_post_publishes_command(monkeypatch):
    fake = FakeIot()
    monkeypatch.setattr(lf, "iot", fake)
    resp = lf.lambda_handler(evt("POST", thing="onair-test-1", mode=1), None)
    assert resp["statusCode"] == 200
    assert fake.published == [{"topic": "onair/onair-test-1/cmd", "qos": 0,
                               "payload": json.dumps({"mode": 1})}]


def test_post_invalid_mode_is_400(monkeypatch):
    monkeypatch.setattr(lf, "iot", FakeIot())
    resp = lf.lambda_handler(evt("POST", thing="onair-test-1", mode=7), None)
    assert resp["statusCode"] == 400
