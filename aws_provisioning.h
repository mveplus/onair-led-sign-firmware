// aws_provisioning.h — Fleet Provisioning by Claim.
//
// One-shot bootstrap flow: the device starts with a shared, low-privilege
// "claim" cert/key, MQTT-connects to the account's AWS IoT endpoint, and
// exchanges the claim cert for a per-device certificate + Thing using the
// $aws/certificates/create and $aws/provisioning-templates topics.
//
// On success the new permanent identity is persisted via awsIotProvision()
// — the persistent MQTT module in aws_iot.cpp picks it up on the next
// loop tick. The claim cert is never written to NVS.

#pragma once

#include <Arduino.h>
#include "aws_iot.h"

#if ENABLE_AWS_IOT

enum AwsClaimResult {
  AWS_CLAIM_OK = 0,
  AWS_CLAIM_BAD_INPUT,
  AWS_CLAIM_NO_WIFI,
  AWS_CLAIM_NTP_FAILED,
  AWS_CLAIM_CONNECT_FAILED,
  AWS_CLAIM_TIMEOUT,
  AWS_CLAIM_REJECTED,
  AWS_CLAIM_INTERNAL_ERROR,
};

// Run the claim flow synchronously. Blocks for up to ~30s under timeout
// (15s per MQTT phase). Safe to call from an AsyncWebServer handler.
//
// On AWS_CLAIM_OK, awsIotProvision() has already been invoked with the
// permanent identity and out_thing_name carries the assigned Thing name.
// On failure, out_error carries a one-line explanation suitable for the
// captive portal status string.
//
// root_ca empty → bundled Amazon Root CA 1 used as the trust anchor.
// parameters empty → {"SerialNumber": "<mac12>"} sent by default;
// otherwise the string is parsed as a JSON object and merged into the
// RegisterThing payload's "parameters" field.
AwsClaimResult awsClaim(const String& endpoint,
                        const String& root_ca,
                        const String& claim_cert,
                        const String& claim_key,
                        const String& template_name,
                        const String& parameters,
                        String& out_thing_name,
                        String& out_error);

const char* awsClaimResultName(AwsClaimResult r);

#endif  // ENABLE_AWS_IOT
