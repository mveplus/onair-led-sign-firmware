# AWS IoT Policies

The provisioning flows in this project expect a small number of AWS IoT
policies to exist in your account. They are intended to be created **once
per account by the AWS admin**, not by the firmware or `scripts/`. The
captive portal then attaches the right policy to each per-device cert.

Two policies cover the cases this project uses:

- `OnAirSignPolicy` — least-privilege per-device policy. Attached to every
  client certificate after provisioning. Scoped to the device's own MQTT
  topics via `${iot:ClientId}` and `${iot:Connection.Thing.ThingName}`.
- `OnAirBootstrapPolicy` *(optional, Phase 3)* — restricted policy attached
  to the shared **claim** certificate used by Fleet Provisioning by Claim.
  Only allows the four AWS provisioning topics so a leaked claim cert can't
  impersonate provisioned devices.

---

## `OnAirSignPolicy`

```jsonc
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": "iot:Connect",
      "Resource": "arn:aws:iot:*:*:client/${iot:ClientId}"
    },
    {
      "Effect": "Allow",
      "Action": ["iot:Publish", "iot:Receive"],
      "Resource": [
        "arn:aws:iot:*:*:topic/onair/${iot:Connection.Thing.ThingName}/*",
        "arn:aws:iot:*:*:topic/$aws/things/${iot:Connection.Thing.ThingName}/shadow/*",
        "arn:aws:iot:*:*:topic/$aws/things/${iot:Connection.Thing.ThingName}/jobs/*"
      ]
    },
    {
      "Effect": "Allow",
      "Action": "iot:Subscribe",
      "Resource": [
        "arn:aws:iot:*:*:topicfilter/onair/${iot:Connection.Thing.ThingName}/cmd",
        "arn:aws:iot:*:*:topicfilter/$aws/things/${iot:Connection.Thing.ThingName}/shadow/*",
        "arn:aws:iot:*:*:topicfilter/$aws/things/${iot:Connection.Thing.ThingName}/jobs/*"
      ]
    }
  ]
}
```

Create it once per account:

```bash
aws iot create-policy \
  --policy-name OnAirSignPolicy \
  --policy-document file://onair-sign-policy.json \
  --region eu-west-1
```

The `scripts/provision-device.sh` script refuses to run unless this policy
already exists in the target region — failing closed avoids accidentally
creating a too-broad policy on first use.

---

## `OnAirBootstrapPolicy` — Fleet Provisioning by Claim

Phase 3 of `PROVISIONING_PLAN.md`. The claim certificate is a shared,
low-privilege identity that bootstraps each device into its own
per-device cert + Thing. Restrict the claim cert's IoT policy to **only**
the AWS provisioning topics so a leaked claim cert is limited to spamming
the provisioning queue (which a Lambda pre-provisioning hook can rate-limit
or allowlist on top of).

Replace `<TEMPLATE_NAME>` with your actual provisioning-template name.

```jsonc
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": "iot:Connect",
      "Resource": "arn:aws:iot:*:*:client/*"
    },
    {
      "Effect": "Allow",
      "Action": ["iot:Publish", "iot:Receive"],
      "Resource": [
        "arn:aws:iot:*:*:topic/$aws/certificates/create/json",
        "arn:aws:iot:*:*:topic/$aws/provisioning-templates/<TEMPLATE_NAME>/provision/json"
      ]
    },
    {
      "Effect": "Allow",
      "Action": "iot:Subscribe",
      "Resource": [
        "arn:aws:iot:*:*:topicfilter/$aws/certificates/create/json/accepted",
        "arn:aws:iot:*:*:topicfilter/$aws/certificates/create/json/rejected",
        "arn:aws:iot:*:*:topicfilter/$aws/provisioning-templates/<TEMPLATE_NAME>/provision/json/accepted",
        "arn:aws:iot:*:*:topicfilter/$aws/provisioning-templates/<TEMPLATE_NAME>/provision/json/rejected"
      ]
    }
  ]
}
```

Recommended hardening:

- A Lambda **pre-provisioning hook** on the template that allowlists
  expected device MACs / serial numbers and rejects everything else.
- Rotate the claim cert periodically; revoke immediately on suspected
  compromise.
- Do not ship the claim cert in firmware. Users enter it once via the
  captive portal during onboarding.
