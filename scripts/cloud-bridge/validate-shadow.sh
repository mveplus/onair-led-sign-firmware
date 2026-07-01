#!/usr/bin/env bash
#
# validate-shadow.sh — prove the cloud shadow-read path end to end WITHOUT
# the device. Seeds a Thing Shadow as if the firmware had reported it,
# confirms AWS stored it, then reads it back through the deployed Lambda
# exactly the way the extension's cloud `verify` will.
#
# Usage:
#   ./validate-shadow.sh <thing-name> [mode]
#   ./validate-shadow.sh onair-test-1 2
#
# Not sure of your thing name? List them:
#   aws iot list-things --profile "$AWS_PROFILE" --query 'things[].thingName' --output table
#
# Env (with defaults):
#   AWS_PROFILE  onair-iot          AWS_REGION   eu-west-1
#   API_NAME     onair-publish-api  TOKEN_FILE   ./.onair-bridge-token
#   API_ENDPOINT (auto-resolved from API_NAME if unset)
#   SHARED_TOKEN (read from TOKEN_FILE if unset)
set -euo pipefail

THING="${1:-}"
MODE="${2:-2}"
if [[ -z "$THING" ]]; then
  echo "usage: $0 <thing-name> [mode]" >&2
  exit 2
fi

AWS_PROFILE="${AWS_PROFILE:-onair-iot}"
AWS_REGION="${AWS_REGION:-eu-west-1}"
API_NAME="${API_NAME:-onair-publish-api}"
TOKEN_FILE="${TOKEN_FILE:-./.onair-bridge-token}"

TOKEN="${SHARED_TOKEN:-$( [[ -f "$TOKEN_FILE" ]] && cat "$TOKEN_FILE" || true )}"
if [[ -z "$TOKEN" ]]; then
  echo "No bearer token — set SHARED_TOKEN or provide $TOKEN_FILE (deploy.sh writes it)." >&2
  exit 1
fi

API_ENDPOINT="${API_ENDPOINT:-}"
if [[ -z "$API_ENDPOINT" ]]; then
  API_ENDPOINT="$(aws apigatewayv2 get-apis --region "$AWS_REGION" --profile "$AWS_PROFILE" \
    --query "Items[?Name=='${API_NAME}'].ApiEndpoint | [0]" --output text)"
fi
if [[ -z "$API_ENDPOINT" || "$API_ENDPOINT" == "None" ]]; then
  echo "Could not resolve API endpoint for '$API_NAME' — set API_ENDPOINT explicitly." >&2
  exit 1
fi

aws() { command aws --region "$AWS_REGION" --profile "$AWS_PROFILE" "$@"; }
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

echo "──────────────────────────────────────────────"
echo "Thing: $THING   Mode: $MODE   API: $API_ENDPOINT"
echo "──────────────────────────────────────────────"

echo "1) Seed shadow (pretending to be the firmware)…"
aws iot-data update-thing-shadow --thing-name "$THING" \
  --cli-binary-format raw-in-base64-out \
  --payload "$(printf '{"state":{"reported":{"mode":%d}}}' "$MODE")" \
  "$tmp/update.json" >/dev/null
echo "   ✓ shadow updated"

echo "2) Read shadow directly from AWS IoT…"
aws iot-data get-thing-shadow --thing-name "$THING" "$tmp/shadow.json" >/dev/null
cat "$tmp/shadow.json"; echo

echo "3) Read it back through the Lambda (as the extension will)…"
code="$(curl -sS -o "$tmp/resp.json" -w '%{http_code}' \
  "${API_ENDPOINT%/}/?thing=${THING}" -H "Authorization: Bearer ${TOKEN}")"
echo "   HTTP $code"
cat "$tmp/resp.json"; echo

if [[ "$code" != "200" ]]; then
  echo "✗ FAIL: expected HTTP 200 from the Lambda" >&2
  exit 1
fi
if python3 -c "import json,sys; d=json.load(open('$tmp/resp.json')); sys.exit(0 if d.get('mode')==$MODE else 1)"; then
  echo "✓ PASS — Lambda returned mode=$MODE from the shadow. Cloud verify path works."
else
  echo "✗ FAIL: Lambda response mode did not match $MODE" >&2
  exit 1
fi
