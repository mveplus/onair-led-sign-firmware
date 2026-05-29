// aws_iot.h — AWS IoT Core MQTT client with runtime-loaded credentials.
//
// All configuration is read from the ESP32 Preferences namespace "aws_iot"
// at awsIotSetup() time. There are no compile-time secrets — the module
// stays idle until provisioned via the captive portal or the
// POST /api/aws/provision endpoint.
//
// Keys in the "aws_iot" Preferences namespace:
//   endpoint  String   account ATS host, e.g. xxxx-ats.iot.<region>.amazonaws.com
//   thing     String   optional; falls back to "onair-<mac12>" at runtime
//   root_ca   String   optional; falls back to bundled Amazon Root CA 1
//   cert      String   PEM, per-device client certificate
//   key       String   PEM, per-device client private key
//
// Topics:
//   onair/<thing>/state   reported state (published on every setOutputMode)
//   onair/<thing>/cmd     subscribed; JSON {"mode": 0|1|2} -> setOutputMode

#pragma once

#include <Arduino.h>

#ifndef ENABLE_AWS_IOT
#define ENABLE_AWS_IOT 0
#endif

// (Re)load configuration from Preferences and connect MQTT. No-op when the
// module is unprovisioned. Safe to call repeatedly.
void awsIotSetup();

// Pump MQTT keep-alive + throttled reconnect. Call from loop().
void awsIotLoop();

// Publish the current output mode (0=off, 1=on, 2=breathing) to
// onair/<thing>/state. No-op when MQTT is not connected.
void awsIotPublishState(int mode);

// Persist a new config to Preferences and re-init the module. Empty
// root_ca → bundled CA used at runtime. Empty thing_name → mac default.
// Returns false when endpoint/cert/key are not all non-empty.
bool awsIotProvision(const String& endpoint,
                     const String& root_ca,
                     const String& thing_name,
                     const String& cert,
                     const String& key);

// Wipe the "aws_iot" Preferences namespace and tear down the connection.
void awsIotForget();

// Module status — safe to call any time.
bool   awsIotIsProvisioned();
bool   awsIotIsConnected();
int    awsIotLastRc();          // last PubSubClient state(); 0 = never tried
String awsIotThingName();       // empty when unprovisioned
String awsIotEndpoint();        // empty when unprovisioned
