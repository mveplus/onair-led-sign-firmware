// aws_iot.cpp — AWS IoT Core MQTT module (scaffold).
//
// This is a stub implementation. The full wiring (PubSubClient +
// WiFiClientSecure, shadow client, jobs handler, OTA via Update) is
// described in PLAN.md and will land in a follow-up commit. This file
// exists so the rest of the firmware can call into AwsIot::* without
// conditional compilation at every call site.

#include "aws_iot.h"

#if ENABLE_AWS_IOT

#include <WiFi.h>
#include <WiFiClientSecure.h>
// PubSubClient is the planned MQTT client (see PLAN.md §3). It is not
// yet a project dependency, so the include is guarded so this scaffold
// still compiles cleanly until the library is added.
#if defined(__has_include)
  #if __has_include(<PubSubClient.h>)
    #include <PubSubClient.h>
    #define AWS_IOT_HAS_PUBSUB 1
  #else
    #define AWS_IOT_HAS_PUBSUB 0
  #endif
#else
  #define AWS_IOT_HAS_PUBSUB 0
#endif

namespace {

// Resolved at begin(); empty until then.
String g_thingName;
bool   g_initialized = false;

// Forward declarations for the not-yet-implemented internal handlers.
// They are referenced from the doc but intentionally not wired up yet.
void handleCommand(const char* topic, const uint8_t* payload, unsigned int len);
void handleShadowDelta(const uint8_t* payload, unsigned int len);
void handleJobNotify(const uint8_t* payload, unsigned int len);

String resolveThingName() {
#ifdef AWS_IOT_THING_NAME
  return String(AWS_IOT_THING_NAME);
#else
  // Fall back to "onair-<mac12>" using the STA MAC.
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  char buf[24];
  snprintf(buf, sizeof(buf), "onair-%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
#endif
}

}  // namespace

namespace AwsIot {

bool begin() {
  if (g_initialized) return true;
#if !defined(AWS_IOT_ENDPOINT) || !defined(AWS_IOT_ROOT_CA) || \
    !defined(AWS_IOT_CLIENT_CERT) || !defined(AWS_IOT_CLIENT_KEY)
  // secrets.h is missing required macros; module stays disabled at runtime.
  return false;
#else
  g_thingName = resolveThingName();
  // TODO: configure WiFiClientSecure with root CA + client cert/key
  // TODO: configure PubSubClient with endpoint + port 8883
  // TODO: subscribe topics: onair/<thing>/cmd, $aws/things/<thing>/shadow/update/delta,
  //       $aws/things/<thing>/jobs/notify-next
  g_initialized = true;
  return true;
#endif
}

bool connect() {
  if (!g_initialized) return false;
#if AWS_IOT_HAS_PUBSUB
  // TODO: pubsub.connect(g_thingName.c_str())
  //       on success → resubscribe topics + publish boot event
  return false;
#else
  return false;
#endif
}

void loop() {
  if (!g_initialized) return;
#if AWS_IOT_HAS_PUBSUB
  // TODO: pubsub.loop(); reconnect with exponential backoff if dropped.
#endif
}

bool publishState(const JsonDocument& /*doc*/) {
  if (!g_initialized) return false;
#if AWS_IOT_HAS_PUBSUB
  // TODO: serialize doc → publish to onair/<thing>/state (QoS 0)
  // TODO: also publish to $aws/things/<thing>/shadow/update with
  //       {"state":{"reported": <doc>}}
  return false;
#else
  return false;
#endif
}

bool isConnected() {
#if AWS_IOT_HAS_PUBSUB
  return false;  // TODO: pubsub.connected()
#else
  return false;
#endif
}

const char* thingName() {
  return g_initialized ? g_thingName.c_str() : nullptr;
}

}  // namespace AwsIot

namespace {

void handleCommand(const char* /*topic*/, const uint8_t* /*payload*/, unsigned int /*len*/) {
  // TODO: parse {"mode":..., "period_ms":..., "min":..., "max":...}
  //       and dispatch to the same applyOutputMode() used by HTTP and WS.
}

void handleShadowDelta(const uint8_t* /*payload*/, unsigned int /*len*/) {
  // TODO: apply desired→reported for any keys we recognize.
}

void handleJobNotify(const uint8_t* /*payload*/, unsigned int /*len*/) {
  // TODO: parse job document; HTTPS GET firmware url; verify sha256;
  //       Update.write(); mark job SUCCEEDED or FAILED.
}

}  // namespace

#else  // !ENABLE_AWS_IOT — empty stubs so call sites compile unconditionally.

namespace AwsIot {
bool begin()                                  { return false; }
bool connect()                                { return false; }
void loop()                                   {}
bool publishState(const JsonDocument&)        { return false; }
bool isConnected()                            { return false; }
const char* thingName()                       { return nullptr; }
}  // namespace AwsIot

#endif  // ENABLE_AWS_IOT
