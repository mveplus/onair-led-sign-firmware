// aws_iot.cpp — minimal AWS IoT Core MQTT client.
//
// PubSubClient over WiFiClientSecure (mbedTLS). One persistent connection
// to the account endpoint on TCP 8883, MQTT 3.1.1, mutual-TLS auth using
// the per-device cert/key in secrets.h.
//
// Topics:
//   onair/<thing>/state   reported state, published on every setOutputMode()
//   onair/<thing>/cmd     subscribed; JSON {"mode": 0|1|2} -> setOutputMode()
//
// Reconnect strategy: passive — awsIotLoop() retries every 5s when the
// MQTT link is down. We never block the main loop.
//
// ENABLE_AWS_IOT comes from the build system (-DENABLE_AWS_IOT=1 in
// scripts/build.sh). We do NOT redefine it here — that would silently
// override the build configuration in a single translation unit and
// turn off the feature only for sketches that don't link this file.

#include "aws_iot.h"

#if ENABLE_AWS_IOT

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "secrets.h"

// Provided by the main sketch.
extern void setOutputMode(int mode);

// Last mode pushed via awsIotPublishState() — also re-published on (re)connect
// so AWS sees a fresh value without the .ino having to poke us again.
static int g_lastPublishedMode = 0;

namespace {

WiFiClientSecure tlsClient;
PubSubClient     mqtt(tlsClient);

// Built once at setup() — concatenating string literals at file scope is
// awkward across translation units, so we build them in awsIotSetup().
String topicState;
String topicCmd;

uint32_t lastReconnectAttempt = 0;
const uint32_t RECONNECT_BACKOFF_MS = 5000;

bool g_timeSynced = false;

// AWS IoT mutual TLS validates the server certificate's notBefore/notAfter
// against the system clock. The ESP32 has no RTC, so without an SNTP sync it
// sits at the 1970 epoch and every handshake fails X.509 date validation.
// Block (briefly) for the first sync; afterwards it's a no-op.
bool ensureTimeSynced() {
  if (g_timeSynced) return true;
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("[AWS IoT] syncing time over NTP");
  time_t now = time(nullptr);
  for (int i = 0; i < 40 && now < 1700000000; ++i) {  // up to ~8s
    delay(200);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println();
  if (now < 1700000000) {
    Serial.println("[AWS IoT] NTP sync failed (clock still at epoch); TLS will fail");
    return false;
  }
  g_timeSynced = true;
  Serial.print("[AWS IoT] time synced: ");
  Serial.println((uint32_t)now);
  return true;
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  Serial.print("[AWS IoT] rx ");
  Serial.print(topic);
  Serial.print(" (");
  Serial.print(length);
  Serial.println(" bytes)");

  if (topicCmd.length() == 0 || strcmp(topic, topicCmd.c_str()) != 0) return;

  StaticJsonDocument<192> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print("[AWS IoT] cmd parse error: ");
    Serial.println(err.c_str());
    return;
  }
  if (!doc.containsKey("mode")) return;

  int mode = doc["mode"].as<int>();
  Serial.print("[AWS IoT] cmd -> setOutputMode(");
  Serial.print(mode);
  Serial.println(")");
  setOutputMode(mode);
  // setOutputMode() already calls awsIotPublishState() so we don't echo here.
}

bool reconnect() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!ensureTimeSynced()) return false;
  Serial.print("[AWS IoT] connecting MQTT as ");
  Serial.print(AWS_IOT_THING_NAME);
  Serial.print(" to ");
  Serial.println(AWS_IOT_ENDPOINT);
  if (mqtt.connect(AWS_IOT_THING_NAME)) {
    Serial.println("[AWS IoT] MQTT connected");
    if (topicCmd.length() > 0) {
      mqtt.subscribe(topicCmd.c_str());
      Serial.print("[AWS IoT] subscribed ");
      Serial.println(topicCmd);
    }
    awsIotPublishState(g_lastPublishedMode);
    return true;
  }
  int rc = mqtt.state();
  Serial.print("[AWS IoT] MQTT connect failed, rc=");
  Serial.println(rc);
  // rc -4 (MQTT_CONNECTION_TIMEOUT) here means the TLS handshake succeeded
  // but AWS closed the link after the MQTT CONNECT without a CONNACK. Usual
  // causes: the endpoint region does not match where the cert/thing are
  // registered, the cert has no attached IoT policy, or the policy denies
  // iot:Connect for this client id.
  if (rc == -4) {
    Serial.println("[AWS IoT]   TLS ok but no CONNACK -> check the endpoint "
                   "region and the IoT policy on this cert (client/" AWS_IOT_THING_NAME ")");
  }
  return false;
}

}  // namespace

void awsIotSetup() {
  topicState = String("onair/") + AWS_IOT_THING_NAME + "/state";
  topicCmd   = String("onair/") + AWS_IOT_THING_NAME + "/cmd";

  tlsClient.setCACert(AWS_IOT_ROOT_CA);
  tlsClient.setCertificate(AWS_IOT_CLIENT_CERT);
  tlsClient.setPrivateKey(AWS_IOT_CLIENT_KEY);

  mqtt.setServer(AWS_IOT_ENDPOINT, AWS_IOT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(60);
  mqtt.setCallback(onMqttMessage);

  reconnect();
  lastReconnectAttempt = millis();
}

void awsIotLoop() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!mqtt.connected()) {
    uint32_t now = millis();
    if (now - lastReconnectAttempt >= RECONNECT_BACKOFF_MS) {
      lastReconnectAttempt = now;
      reconnect();
    }
    return;
  }
  mqtt.loop();
}

void awsIotPublishState(int mode) {
  g_lastPublishedMode = mode;
  if (!mqtt.connected()) return;
  StaticJsonDocument<160> doc;
  doc["mode"]      = mode;
  doc["thing"]     = AWS_IOT_THING_NAME;
  doc["uptime_ms"] = (uint32_t)millis();
  doc["rssi"]      = WiFi.RSSI();
  char buf[192];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(topicState.c_str(), reinterpret_cast<const uint8_t*>(buf), n, false);
}

#else  // !ENABLE_AWS_IOT — keep the symbols defined so the linker is happy
       // even if this TU is compiled with the feature off.

void awsIotSetup() {}
void awsIotLoop() {}
void awsIotPublishState(int /*mode*/) {}

#endif  // ENABLE_AWS_IOT
