#!/usr/bin/env bash
#
# deploy.sh — Deploy the OnAir cloud bridge.
#
# Provisions / updates a Lambda function that publishes to onair/<thing>/cmd
# over AWS IoT, fronts it with an API Gateway HTTP API, and emits the
# endpoint + bearer token the Chrome extension needs.
#
# Idempotent: re-running updates the Lambda code and configuration,
# reuses the IAM role and API Gateway if they already exist.
#
# Usage:
#   AWS_PROFILE=onair-iot ./scripts/cloud-bridge/deploy.sh
#
# Environment overrides:
#   AWS_REGION         (default: eu-west-1)
#   FUNCTION_NAME      (default: onair-publish)
#   ROLE_NAME          (default: onair-publish-role)
#   API_NAME           (default: onair-publish-api)
#   ALLOWED_THINGS     (default: onair-test-1) — comma-separated allowlist
#   TOKEN_FILE         (default: ./.onair-bridge-token) — bearer token cache

set -euo pipefail

AWS_REGION="${AWS_REGION:-eu-west-1}"
FUNCTION_NAME="${FUNCTION_NAME:-onair-publish}"
ROLE_NAME="${ROLE_NAME:-onair-publish-role}"
API_NAME="${API_NAME:-onair-publish-api}"
ALLOWED_THINGS="${ALLOWED_THINGS:-onair-test-1}"
TOKEN_FILE="${TOKEN_FILE:-./.onair-bridge-token}"

for bin in aws jq openssl zip; do
  if ! command -v "$bin" >/dev/null 2>&1; then
    echo "ERROR: required tool '$bin' not found in PATH" >&2
    exit 1
  fi
done

if ! aws sts get-caller-identity --region "$AWS_REGION" >/dev/null 2>&1; then
  echo "ERROR: AWS credentials not configured (aws sts get-caller-identity failed)" >&2
  echo "       Set AWS_PROFILE or run: aws configure / aws sso login" >&2
  exit 2
fi

ACCOUNT_ID="$(aws sts get-caller-identity --query Account --output text --region "$AWS_REGION")"
echo "Account: $ACCOUNT_ID  Region: $AWS_REGION"

# ---- bearer token --------------------------------------------------------
if [[ -f "$TOKEN_FILE" ]]; then
  SHARED_TOKEN="$(cat "$TOKEN_FILE")"
  echo "Re-using existing bearer token from $TOKEN_FILE"
else
  SHARED_TOKEN="$(openssl rand -hex 24)"
  printf '%s' "$SHARED_TOKEN" > "$TOKEN_FILE"
  chmod 600 "$TOKEN_FILE"
  echo "Generated fresh bearer token; stashed at $TOKEN_FILE (chmod 600)"
fi

# ---- IAM role ------------------------------------------------------------
TRUST_DOC='{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"Service":"lambda.amazonaws.com"},"Action":"sts:AssumeRole"}]}'

if aws iam get-role --role-name "$ROLE_NAME" >/dev/null 2>&1; then
  echo "IAM role $ROLE_NAME exists, reusing."
else
  echo "Creating IAM role $ROLE_NAME"
  aws iam create-role \
    --role-name "$ROLE_NAME" \
    --assume-role-policy-document "$TRUST_DOC" >/dev/null
  aws iam attach-role-policy \
    --role-name "$ROLE_NAME" \
    --policy-arn arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole
  echo "Waiting 10 s for IAM role propagation…"
  sleep 10
fi

# Always (re)apply the inline IoT policy — least-privilege, and idempotent
# so re-running deploy upgrades an existing role with new permissions:
#   iot:Publish        onair/<thing>/cmd   — send commands to devices
#   iot:GetThingShadow thing/onair-*       — read reported state for the
#                                            extension's cloud `verify`
IOT_DOC="$(jq -n \
  --arg cmd "arn:aws:iot:${AWS_REGION}:${ACCOUNT_ID}:topic/onair/*/cmd" \
  --arg thing "arn:aws:iot:${AWS_REGION}:${ACCOUNT_ID}:thing/onair-*" \
  '{Version:"2012-10-17",Statement:[
     {Effect:"Allow",Action:"iot:Publish",Resource:$cmd},
     {Effect:"Allow",Action:"iot:GetThingShadow",Resource:$thing}
   ]}')"
aws iam put-role-policy \
  --role-name "$ROLE_NAME" \
  --policy-name iot-publish-onair \
  --policy-document "$IOT_DOC"
ROLE_ARN="arn:aws:iam::${ACCOUNT_ID}:role/${ROLE_NAME}"

# ---- Lambda zip ----------------------------------------------------------
LAMBDA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# mktemp -t with a .zip suffix creates an empty 0-byte file, and `zip`
# then refuses to "update" the empty non-archive. Use a temp dir and put
# the archive inside it instead.
ZIP_DIR="$(mktemp -d -t onair-lambda.XXXXXX)"
ZIP_FILE="$ZIP_DIR/lambda.zip"
trap 'rm -rf "$ZIP_DIR"' EXIT
( cd "$LAMBDA_DIR" && zip -q "$ZIP_FILE" lambda_function.py )

# Lambda's reserved env list does NOT permit AWS_REGION explicitly; the
# function picks up the runtime's region automatically, so we omit it here
# and only ship SHARED_TOKEN + ALLOWED_THINGS.
# Build as JSON, not CLI shorthand: shorthand (Variables={K=V,...}) uses
# commas as the pair separator, so a multi-value ALLOWED_THINGS like
# "onair-test-1,onair-office" would be misparsed. JSON keeps it intact.
ENV_VARS="$(jq -n \
  --arg token "$SHARED_TOKEN" \
  --arg allowed "$ALLOWED_THINGS" \
  '{Variables:{SHARED_TOKEN:$token, ALLOWED_THINGS:$allowed}}')"

# ---- Lambda function ----------------------------------------------------
if aws lambda get-function --function-name "$FUNCTION_NAME" --region "$AWS_REGION" >/dev/null 2>&1; then
  echo "Updating Lambda function $FUNCTION_NAME"
  aws lambda update-function-code \
    --function-name "$FUNCTION_NAME" \
    --zip-file "fileb://$ZIP_FILE" \
    --region "$AWS_REGION" >/dev/null
  aws lambda wait function-updated \
    --function-name "$FUNCTION_NAME" \
    --region "$AWS_REGION"
  aws lambda update-function-configuration \
    --function-name "$FUNCTION_NAME" \
    --environment "$ENV_VARS" \
    --region "$AWS_REGION" >/dev/null
else
  echo "Creating Lambda function $FUNCTION_NAME"
  aws lambda create-function \
    --function-name "$FUNCTION_NAME" \
    --runtime python3.12 \
    --handler lambda_function.lambda_handler \
    --role "$ROLE_ARN" \
    --zip-file "fileb://$ZIP_FILE" \
    --environment "$ENV_VARS" \
    --timeout 5 \
    --region "$AWS_REGION" >/dev/null
fi
LAMBDA_ARN="arn:aws:lambda:${AWS_REGION}:${ACCOUNT_ID}:function:${FUNCTION_NAME}"

# ---- API Gateway HTTP API -----------------------------------------------
API_ID="$(aws apigatewayv2 get-apis --region "$AWS_REGION" \
  --query "Items[?Name=='${API_NAME}'].ApiId | [0]" --output text)"

if [[ "$API_ID" == "None" || -z "$API_ID" ]]; then
  echo "Creating API Gateway HTTP API $API_NAME"
  API_ID="$(aws apigatewayv2 create-api \
    --name "$API_NAME" \
    --protocol-type HTTP \
    --target "$LAMBDA_ARN" \
    --cors-configuration "AllowOrigins=*,AllowHeaders=Authorization,Content-Type,AllowMethods=POST,OPTIONS" \
    --region "$AWS_REGION" \
    --query ApiId --output text)"
  aws lambda add-permission \
    --function-name "$FUNCTION_NAME" \
    --statement-id "apigw-invoke-${API_ID}" \
    --action lambda:InvokeFunction \
    --principal apigateway.amazonaws.com \
    --source-arn "arn:aws:execute-api:${AWS_REGION}:${ACCOUNT_ID}:${API_ID}/*" \
    --region "$AWS_REGION" >/dev/null
else
  echo "API Gateway $API_NAME exists ($API_ID), reusing."
fi

API_ENDPOINT="$(aws apigatewayv2 get-api --api-id "$API_ID" --region "$AWS_REGION" \
  --query ApiEndpoint --output text)"

# Use the first allowlisted thing in the examples below. Hardcoding a name
# the deployer doesn't actually own makes the smoke test return 200 while
# nothing happens (it publishes to a topic no device subscribes to).
EXAMPLE_THING="${ALLOWED_THINGS%%,*}"

# ---- Output --------------------------------------------------------------
cat <<EOF

Done.

  API endpoint   : $API_ENDPOINT
  Function       : $FUNCTION_NAME  (region $AWS_REGION)
  Allowed things : $ALLOWED_THINGS
  Token cached at: $TOKEN_FILE  (do not commit; .gitignored by default)

Smoke test (off → on → breathing → off):

  TOKEN=\$(cat $TOKEN_FILE)
  for m in 0 1 2 0; do
    curl -fsS -X POST "$API_ENDPOINT/onair" \\
      -H "Authorization: Bearer \$TOKEN" \\
      -H "Content-Type: application/json" \\
      -d "{\"thing\":\"$EXAMPLE_THING\",\"mode\":\$m}"
    echo
    sleep 2
  done

A 200 {"ok":true} only means the publish succeeded. If the sign does not
move, the thing name must match a device actually subscribed to
onair/<thing>/cmd — not just any name in ALLOWED_THINGS.

Chrome extension settings:
  cloudBase  = $API_ENDPOINT
  cloudToken = (value from $TOKEN_FILE)
  thing      = $EXAMPLE_THING
EOF
