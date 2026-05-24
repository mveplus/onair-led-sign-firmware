// aws_iot.h — Minimal AWS IoT Core MQTT client (Week 2 scaffold).
//
// Public surface is intentionally tiny:
//   awsIotSetup()         — call after WiFi STA is up; loads certs and
//                           attempts the first MQTT connection.
//   awsIotLoop()          — call from the main loop(); handles MQTT
//                           keep-alive plus throttled reconnect.
//   awsIotPublishState()  — publish current outputMode to
//                           onair/<thing>/state.
//
// Commands arrive on onair/<thing>/cmd as JSON: {"mode": 0|1|2}, and are
// dispatched to the existing setOutputMode() in the main sketch.
//
// The .ino call sites are wrapped in `#ifdef ENABLE_AWS_IOT` so the feature
// can be disabled from the sketch without touching this file.

#pragma once

#include <Arduino.h>

void awsIotSetup();
void awsIotLoop();
// Publish the current mode (0=off, 1=on, 2=breathing) to onair/<thing>/state.
// The caller passes the value to avoid reaching across translation units;
// the main sketch keeps `outputMode` as a static (internal linkage) global.
void awsIotPublishState(int mode);
