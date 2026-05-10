// aws_iot.h — AWS IoT Core MQTT module (scaffold).
//
// Compile-time gated by ENABLE_AWS_IOT (defined in secrets.h or via
// build flags). When ENABLE_AWS_IOT is 0 or undefined every entry point
// is a no-op so the rest of the firmware can call AwsIot::loop() etc.
// unconditionally.
//
// The full design and topic plan live in PLAN.md.

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// secrets.h is gitignored. It is generated per build and provides:
//   #define ENABLE_AWS_IOT 1
//   #define AWS_IOT_ENDPOINT      "xxxx-ats.iot.eu-west-1.amazonaws.com"
//   #define AWS_IOT_THING_NAME    "onair-<mac>"     // optional, falls back to hostname
//   extern const char AWS_IOT_ROOT_CA[];            // PEM
//   extern const char AWS_IOT_CLIENT_CERT[];        // PEM
//   extern const char AWS_IOT_CLIENT_KEY[];         // PEM
#if defined(__has_include)
  #if __has_include("secrets.h")
    #include "secrets.h"
  #endif
#endif

#ifndef ENABLE_AWS_IOT
#define ENABLE_AWS_IOT 0
#endif

namespace AwsIot {

// Load configuration (from secrets.h and/or NVS) and prepare TLS state.
// Returns false if AWS IoT is disabled or required config is missing.
// Does not touch the network.
bool begin();

// (Re)connect to AWS IoT Core. Call when WiFi STA is up.
// Returns true on success, false on failure (caller may retry later).
bool connect();

// Pump MQTT and handle reconnect with backoff. Cheap; safe to call from loop().
void loop();

// Publish current device state to onair/<thing>/state and to the
// classic Thing Shadow as `reported`. Returns false if not connected.
bool publishState(const JsonDocument& doc);

// True iff the MQTT connection is currently up.
bool isConnected();

// The thing name in use (resolved from secrets.h, NVS, or hostname).
// Returns nullptr when ENABLE_AWS_IOT is 0.
const char* thingName();

}  // namespace AwsIot
