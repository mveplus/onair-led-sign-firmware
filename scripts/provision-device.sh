#!/usr/bin/env bash
#
# provision-device.sh — Create one AWS IoT Thing for an OnAir LED sign.
#
# Creates:
#   1. An IoT Thing named $THING_NAME (default: onair-<random-12-hex>).
#   2. A new X.509 keypair + certificate (active).
#   3. Attaches the certificate to the Thing.
#   4. Attaches the OnAirSignPolicy IAM policy to the certificate.
#   5. Writes cert/key/CA to certs/$THING_NAME/ (gitignored).
#
# Requirements: awscli v2, openssl (only for the random hex name), jq.
# AWS credentials must already be configured (env, profile, or SSO).
#
# Usage:
#   ./scripts/provision-device.sh                          # auto-name
#   ./scripts/provision-device.sh onair-kitchen            # explicit name
#   THING_GROUP=OnAirSigns AWS_REGION=eu-west-1 \
#     ./scripts/provision-device.sh onair-kitchen
#
# Environment overrides:
#   AWS_REGION    AWS region                      (default: eu-west-1)
#   POLICY_NAME   IoT policy to attach            (default: OnAirSignPolicy)
#   THING_GROUP   optional Thing Group to join    (default: unset → no group)
#   CERTS_DIR     where to write cert files       (default: ./certs)
#   ROOT_CA_URL   Amazon Root CA 1 download URL   (default: official AWS URL)

set -euo pipefail

AWS_REGION="${AWS_REGION:-eu-west-1}"
POLICY_NAME="${POLICY_NAME:-OnAirSignPolicy}"
CERTS_DIR="${CERTS_DIR:-certs}"
ROOT_CA_URL="${ROOT_CA_URL:-https://www.amazontrust.com/repository/AmazonRootCA1.pem}"

# ---- pick a thing name ------------------------------------------------------
if [[ $# -ge 1 && -n "$1" ]]; then
  THING_NAME="$1"
else
  rand_hex="$(openssl rand -hex 6)"
  THING_NAME="onair-${rand_hex}"
fi

if ! [[ "$THING_NAME" =~ ^[a-zA-Z0-9_:-]{1,128}$ ]]; then
  echo "ERROR: thing name '$THING_NAME' is not a valid AWS IoT Thing name" >&2
  exit 2
fi

# ---- preflight --------------------------------------------------------------
for bin in aws jq curl; do
  if ! command -v "$bin" >/dev/null 2>&1; then
    echo "ERROR: required tool '$bin' not found in PATH" >&2
    exit 3
  fi
done

if ! aws sts get-caller-identity --region "$AWS_REGION" >/dev/null 2>&1; then
  echo "ERROR: AWS credentials are not configured (aws sts get-caller-identity failed)" >&2
  echo "       Configure with: aws configure  OR  aws sso login --profile <name>" >&2
  exit 4
fi

# Ensure the IoT policy exists; if not, refuse rather than silently create one
# with too-broad permissions. The policy is intended to be created once per
# account by the AWS admin (see PLAN.md §4).
if ! aws iot get-policy --policy-name "$POLICY_NAME" --region "$AWS_REGION" >/dev/null 2>&1; then
  echo "ERROR: IoT policy '$POLICY_NAME' does not exist in region '$AWS_REGION'." >&2
  echo "       Create it once from the JSON in PLAN.md §4 before running this script." >&2
  exit 5
fi

OUT_DIR="$CERTS_DIR/$THING_NAME"
if [[ -d "$OUT_DIR" ]]; then
  echo "ERROR: $OUT_DIR already exists — refusing to overwrite an existing device." >&2
  echo "       Move/delete it first if you really want to re-provision." >&2
  exit 6
fi
mkdir -p "$OUT_DIR"

CERT_PEM="$OUT_DIR/${THING_NAME}.cert.pem"
PRIV_KEY="$OUT_DIR/${THING_NAME}.private.key"
PUB_KEY="$OUT_DIR/${THING_NAME}.public.key"
ROOT_CA="$OUT_DIR/AmazonRootCA1.pem"
META_JSON="$OUT_DIR/thing.json"

echo "[1/5] Creating Thing: $THING_NAME (region $AWS_REGION)"
aws iot create-thing \
  --thing-name "$THING_NAME" \
  --region "$AWS_REGION" \
  >"$META_JSON"

echo "[2/5] Creating + activating keypair and certificate"
CREATE_OUT="$(aws iot create-keys-and-certificate \
  --set-as-active \
  --region "$AWS_REGION")"

CERT_ARN="$(echo "$CREATE_OUT" | jq -r '.certificateArn')"
CERT_ID="$(echo "$CREATE_OUT" | jq -r '.certificateId')"

echo "$CREATE_OUT" | jq -r '.certificatePem'        > "$CERT_PEM"
echo "$CREATE_OUT" | jq -r '.keyPair.PrivateKey'    > "$PRIV_KEY"
echo "$CREATE_OUT" | jq -r '.keyPair.PublicKey'     > "$PUB_KEY"
chmod 600 "$PRIV_KEY"

echo "    certificateId : $CERT_ID"
echo "    certificateArn: $CERT_ARN"

# Stash the IDs alongside the keys so the unprovision flow can find them later.
jq -n \
  --arg thing "$THING_NAME" \
  --arg arn "$CERT_ARN" \
  --arg id "$CERT_ID" \
  --arg region "$AWS_REGION" \
  --arg policy "$POLICY_NAME" \
  '{thing_name:$thing, certificate_arn:$arn, certificate_id:$id, region:$region, policy:$policy}' \
  > "$OUT_DIR/cert-meta.json"

echo "[3/5] Attaching certificate to Thing"
aws iot attach-thing-principal \
  --thing-name "$THING_NAME" \
  --principal "$CERT_ARN" \
  --region "$AWS_REGION"

echo "[4/5] Attaching policy '$POLICY_NAME' to certificate"
aws iot attach-policy \
  --policy-name "$POLICY_NAME" \
  --target "$CERT_ARN" \
  --region "$AWS_REGION"

if [[ -n "${THING_GROUP:-}" ]]; then
  echo "[4b]  Adding Thing to group '$THING_GROUP'"
  aws iot add-thing-to-thing-group \
    --thing-group-name "$THING_GROUP" \
    --thing-name "$THING_NAME" \
    --region "$AWS_REGION"
fi

echo "[5/5] Downloading Amazon Root CA 1"
curl -fsSL "$ROOT_CA_URL" -o "$ROOT_CA"

# Surface the IoT data endpoint so flashing tools can configure secrets.h.
ENDPOINT="$(aws iot describe-endpoint \
  --endpoint-type iot:Data-ATS \
  --region "$AWS_REGION" \
  --query 'endpointAddress' --output text)"

cat <<EOF

Done.

  Thing name : $THING_NAME
  Endpoint   : $ENDPOINT
  Region     : $AWS_REGION
  Files in   : $OUT_DIR/
    - $(basename "$CERT_PEM")
    - $(basename "$PRIV_KEY")    (chmod 600)
    - $(basename "$PUB_KEY")
    - $(basename "$ROOT_CA")
    - cert-meta.json

Next: copy these into secrets.h (escaped as C string literals) and rebuild.
EOF
