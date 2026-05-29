// aws_iot.cpp — AWS IoT Core MQTT client with runtime-loaded credentials.
//
// Config lives in Preferences namespace "aws_iot" (see aws_iot.h for keys).
// awsIotSetup() may be called repeatedly (e.g. after a fresh provisioning);
// every call tears down any existing connection, reloads config from NVS,
// and attempts to reconnect.

#include "aws_iot.h"

#if ENABLE_AWS_IOT

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

// Provided by the main sketch.
extern void setOutputMode(int mode);

namespace {

// Amazon Root CA 1 — public certificate. Source:
// https://www.amazontrust.com/repository/AmazonRootCA1.pem
// Bundled so the user doesn't have to paste it. Overridable at runtime via
// the "root_ca" Preferences key (POST /api/aws/provision body).
const char kAmazonRootCa1[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\n"
"ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\n"
"b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\n"
"MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\n"
"b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\n"
"ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\n"
"9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\n"
"IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\n"
"VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\n"
"93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\n"
"jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\n"
"AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\n"
"A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\n"
"U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\n"
"N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\n"
"o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\n"
"5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\n"
"rqXRfboQnoZsG4q5WTP468SQvvG5\n"
"-----END CERTIFICATE-----\n";

WiFiClientSecure tlsClient;
PubSubClient     mqtt(tlsClient);

// Owned copies of the PEM strings — WiFiClientSecure stores pointers, not
// copies, so these must outlive the TLS session.
String g_caBuf;
String g_certBuf;
String g_keyBuf;

String g_thingName;
String g_endpoint;
String topicState;
String topicCmd;

uint32_t lastReconnectAttempt = 0;
const uint32_t RECONNECT_BACKOFF_MS = 5000;

bool g_timeSynced  = false;
bool g_provisioned = false;
int  g_lastRc      = 0;
int  g_lastPublishedMode = 0;

String defaultThingName() {
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  char buf[24];
  snprintf(buf, sizeof(buf), "onair-%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

bool loadConfigFromPrefs() {
  Preferences p;
  if (!p.begin("aws_iot", true)) return false;
  g_endpoint  = p.getString("endpoint", "");
  g_thingName = p.getString("thing", "");
  String userCa = p.getString("root_ca", "");
  g_certBuf   = p.getString("cert", "");
  g_keyBuf    = p.getString("key", "");
  p.end();

  if (g_endpoint.length() == 0 || g_certBuf.length() == 0 || g_keyBuf.length() == 0) {
    return false;
  }
  if (g_thingName.length() == 0) g_thingName = defaultThingName();
  g_caBuf = (userCa.length() > 0) ? userCa : String(kAmazonRootCa1);
  return true;
}

// AWS IoT mutual TLS validates the server certificate's notBefore/notAfter
// against the system clock. The ESP32 has no RTC, so without an SNTP sync it
// sits at the 1970 epoch and every handshake fails X.509 date validation.
bool ensureTimeSynced() {
  if (g_timeSynced) return true;
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("[AWS IoT] syncing time over NTP");
  time_t now = time(nullptr);
  for (int i = 0; i < 40 && now < 1700000000; ++i) {
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
}

bool reconnect() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!g_provisioned) return false;
  if (!ensureTimeSynced()) return false;
  Serial.print("[AWS IoT] connecting MQTT as ");
  Serial.print(g_thingName);
  Serial.print(" to ");
  Serial.println(g_endpoint);
  if (mqtt.connect(g_thingName.c_str())) {
    Serial.println("[AWS IoT] MQTT connected");
    g_lastRc = 0;
    if (topicCmd.length() > 0) {
      mqtt.subscribe(topicCmd.c_str());
      Serial.print("[AWS IoT] subscribed ");
      Serial.println(topicCmd);
    }
    awsIotPublishState(g_lastPublishedMode);
    return true;
  }
  g_lastRc = mqtt.state();
  Serial.print("[AWS IoT] MQTT connect failed, rc=");
  Serial.println(g_lastRc);
  // rc -4 (MQTT_CONNECTION_TIMEOUT) here means the TLS handshake succeeded
  // but AWS closed the link after the MQTT CONNECT without a CONNACK. Usual
  // causes: the endpoint region does not match where the cert/thing are
  // registered, the cert has no attached IoT policy, or the policy denies
  // iot:Connect for this client id.
  if (g_lastRc == -4) {
    Serial.print("[AWS IoT]   TLS ok but no CONNACK -> check the endpoint "
                 "region and the IoT policy on this cert (client/");
    Serial.print(g_thingName);
    Serial.println(")");
  }
  return false;
}

void teardownConnection() {
  if (mqtt.connected()) mqtt.disconnect();
  g_provisioned = false;
  g_thingName   = "";
  g_endpoint    = "";
  topicState    = "";
  topicCmd      = "";
}

}  // namespace

void awsIotSetup() {
  teardownConnection();

  if (!loadConfigFromPrefs()) {
    Serial.println("[AWS IoT] not provisioned — module idle");
    return;
  }
  g_provisioned = true;

  topicState = String("onair/") + g_thingName + "/state";
  topicCmd   = String("onair/") + g_thingName + "/cmd";

  tlsClient.setCACert(g_caBuf.c_str());
  tlsClient.setCertificate(g_certBuf.c_str());
  tlsClient.setPrivateKey(g_keyBuf.c_str());

  mqtt.setServer(g_endpoint.c_str(), 8883);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(60);
  mqtt.setCallback(onMqttMessage);

  reconnect();
  lastReconnectAttempt = millis();
}

void awsIotLoop() {
  if (!g_provisioned) return;
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
  doc["thing"]     = g_thingName;
  doc["uptime_ms"] = (uint32_t)millis();
  doc["rssi"]      = WiFi.RSSI();
  char buf[192];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(topicState.c_str(), reinterpret_cast<const uint8_t*>(buf), n, false);
}

bool awsIotProvision(const String& endpoint,
                     const String& root_ca,
                     const String& thing_name,
                     const String& cert,
                     const String& key) {
  if (endpoint.length() == 0 || cert.length() == 0 || key.length() == 0) {
    return false;
  }
  Preferences p;
  if (!p.begin("aws_iot", false)) return false;
  p.putString("endpoint", endpoint);
  p.putString("root_ca",  root_ca);    // empty stored → load falls back to bundled CA
  p.putString("thing",    thing_name);
  p.putString("cert",     cert);
  p.putString("key",      key);
  p.end();
  awsIotSetup();
  return true;
}

void awsIotForget() {
  teardownConnection();
  Preferences p;
  if (p.begin("aws_iot", false)) {
    p.clear();
    p.end();
  }
}

bool awsIotIsProvisioned() { return g_provisioned; }
bool awsIotIsConnected()   { return g_provisioned && mqtt.connected(); }
int  awsIotLastRc()        { return g_lastRc; }
String awsIotThingName()   { return g_thingName; }
String awsIotEndpoint()    { return g_endpoint; }

#else  // !ENABLE_AWS_IOT — empty stubs so the linker is happy.

void   awsIotSetup() {}
void   awsIotLoop()  {}
void   awsIotPublishState(int) {}

bool   awsIotProvision(const String&, const String&, const String&,
                       const String&, const String&) { return false; }
void   awsIotForget() {}

bool   awsIotIsProvisioned() { return false; }
bool   awsIotIsConnected()   { return false; }
int    awsIotLastRc()        { return 0; }
String awsIotThingName()     { return String(); }
String awsIotEndpoint()      { return String(); }

#endif  // ENABLE_AWS_IOT
