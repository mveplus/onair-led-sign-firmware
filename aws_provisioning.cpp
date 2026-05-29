// aws_provisioning.cpp — Fleet Provisioning by Claim implementation.
//
// State machine driven by MQTT pub/sub against the AWS IoT provisioning
// topics. Uses its own WiFiClientSecure + PubSubClient (local to the
// awsClaim() call). The persistent AWS module in aws_iot.cpp is left
// alone during the exchange; it picks up the new identity on the next
// awsIotSetup() triggered by awsIotProvision().

#include "aws_provisioning.h"

#if ENABLE_AWS_IOT

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>

namespace {

enum ClaimPhase {
  PHASE_IDLE,
  PHASE_WAIT_CERT,
  PHASE_GOT_CERT,
  PHASE_WAIT_REGISTER,
  PHASE_DONE_OK,
  PHASE_DONE_ERR,
};

// Static state — the MQTT callback can't see locals, so we park the
// per-invocation state here and reset it at the top of awsClaim().
ClaimPhase g_phase = PHASE_IDLE;
String     g_token;       // certificateOwnershipToken from create/accepted
String     g_newCert;     // PEM, from create/accepted
String     g_newKey;      // PEM, from create/accepted
String     g_newThing;    // thingName, from provision/accepted
String     g_errorMsg;
String     g_currentTemplate;
bool       g_timeSynced = false;

bool ensureTimeSynced() {
  if (g_timeSynced) return true;
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  time_t now = time(nullptr);
  for (int i = 0; i < 40 && now < 1700000000; ++i) {
    delay(200);
    now = time(nullptr);
  }
  if (now < 1700000000) return false;
  g_timeSynced = true;
  return true;
}

void onClaimMessage(char* topic, byte* payload, unsigned int length) {
  String t(topic);

  if (t == "$aws/certificates/create/json/accepted") {
    // Response carries certificatePem (~1.2 KB) + privateKey (~1.7 KB) +
    // ownership token + certificateId. Allocate generously.
    DynamicJsonDocument doc(8192);
    if (deserializeJson(doc, payload, length)) {
      g_errorMsg = "create/accepted: invalid JSON";
      g_phase = PHASE_DONE_ERR;
      return;
    }
    g_newCert = doc["certificatePem"]            | "";
    g_newKey  = doc["privateKey"]                | "";
    g_token   = doc["certificateOwnershipToken"] | "";
    if (g_newCert.length() == 0 || g_newKey.length() == 0 || g_token.length() == 0) {
      g_errorMsg = "create/accepted: missing fields";
      g_phase = PHASE_DONE_ERR;
      return;
    }
    g_phase = PHASE_GOT_CERT;
    return;
  }
  if (t == "$aws/certificates/create/json/rejected") {
    DynamicJsonDocument doc(512);
    deserializeJson(doc, payload, length);
    String msg = doc["errorMessage"] | "unknown";
    g_errorMsg = String("create cert rejected: ") + msg;
    g_phase = PHASE_DONE_ERR;
    return;
  }

  // Template-specific topics — built at runtime from g_currentTemplate.
  String acceptedTopic = String("$aws/provisioning-templates/") + g_currentTemplate + "/provision/json/accepted";
  String rejectedTopic = String("$aws/provisioning-templates/") + g_currentTemplate + "/provision/json/rejected";
  if (t == acceptedTopic) {
    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, payload, length)) {
      g_errorMsg = "register/accepted: invalid JSON";
      g_phase = PHASE_DONE_ERR;
      return;
    }
    g_newThing = doc["thingName"] | "";
    if (g_newThing.length() == 0) {
      g_errorMsg = "register/accepted: missing thingName";
      g_phase = PHASE_DONE_ERR;
      return;
    }
    g_phase = PHASE_DONE_OK;
    return;
  }
  if (t == rejectedTopic) {
    DynamicJsonDocument doc(512);
    deserializeJson(doc, payload, length);
    String msg = doc["errorMessage"] | "unknown";
    g_errorMsg = String("register thing rejected: ") + msg;
    g_phase = PHASE_DONE_ERR;
    return;
  }
}

// Spin mqtt.loop() until g_phase moves off `pending` or the deadline.
// Returns true on transition, false on timeout.
bool waitForPhase(PubSubClient& mqtt, ClaimPhase pending, uint32_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  while (g_phase == pending && (int32_t)(deadline - millis()) > 0) {
    mqtt.loop();
    delay(50);
  }
  return g_phase != pending;
}

}  // namespace

AwsClaimResult awsClaim(const String& endpoint,
                        const String& root_ca,
                        const String& claim_cert,
                        const String& claim_key,
                        const String& template_name,
                        const String& parameters,
                        String& out_thing_name,
                        String& out_error) {
  if (endpoint.length() == 0 || claim_cert.length() == 0 ||
      claim_key.length() == 0 || template_name.length() == 0) {
    out_error = "endpoint, claim_cert, claim_key, template_name required";
    return AWS_CLAIM_BAD_INPUT;
  }
  if (WiFi.status() != WL_CONNECTED) {
    out_error = "Wi-Fi not connected";
    return AWS_CLAIM_NO_WIFI;
  }
  if (!ensureTimeSynced()) {
    out_error = "NTP sync failed (TLS handshake would fail)";
    return AWS_CLAIM_NTP_FAILED;
  }

  // CA: user override falls back to bundled Amazon Root CA 1.
  String caBuf = (root_ca.length() > 0) ? root_ca : String(awsIotBundledRootCa1());

  WiFiClientSecure tls;
  PubSubClient     mqtt(tls);
  tls.setCACert(caBuf.c_str());
  tls.setCertificate(claim_cert.c_str());
  tls.setPrivateKey(claim_key.c_str());

  mqtt.setServer(endpoint.c_str(), 8883);
  // create/accepted payload can run ~4 KB; size the read buffer generously.
  mqtt.setBufferSize(8192);
  mqtt.setKeepAlive(60);
  mqtt.setCallback(onClaimMessage);

  // Reset per-invocation static state.
  g_phase = PHASE_IDLE;
  g_token = "";
  g_newCert = "";
  g_newKey = "";
  g_newThing = "";
  g_errorMsg = "";
  g_currentTemplate = template_name;

  char clientId[24];
  snprintf(clientId, sizeof(clientId), "claim-%08x", (uint32_t)esp_random());

  Serial.print("[AWS Claim] connecting MQTT to ");
  Serial.print(endpoint);
  Serial.print(" as ");
  Serial.println(clientId);
  if (!mqtt.connect(clientId)) {
    int rc = mqtt.state();
    out_error = String("MQTT connect failed, rc=") + rc;
    if (rc == -4) out_error += " (TLS ok but no CONNACK; check claim cert IoT policy)";
    return AWS_CLAIM_CONNECT_FAILED;
  }

  // ----- Phase 1: create a fresh device cert from CSR-less request ----
  if (!mqtt.subscribe("$aws/certificates/create/json/accepted") ||
      !mqtt.subscribe("$aws/certificates/create/json/rejected")) {
    out_error = "subscribe (create topics) failed";
    mqtt.disconnect();
    return AWS_CLAIM_CONNECT_FAILED;
  }
  g_phase = PHASE_WAIT_CERT;
  if (!mqtt.publish("$aws/certificates/create/json", "{}")) {
    out_error = "publish (create cert) failed";
    mqtt.disconnect();
    return AWS_CLAIM_CONNECT_FAILED;
  }
  if (!waitForPhase(mqtt, PHASE_WAIT_CERT, 15000)) {
    out_error = "timeout waiting for cert creation";
    mqtt.disconnect();
    return AWS_CLAIM_TIMEOUT;
  }
  if (g_phase == PHASE_DONE_ERR) {
    out_error = g_errorMsg;
    mqtt.disconnect();
    return AWS_CLAIM_REJECTED;
  }
  // g_phase == PHASE_GOT_CERT

  // ----- Phase 2: register the Thing using the new cert + template ----
  String acceptedTopic = String("$aws/provisioning-templates/") + template_name + "/provision/json/accepted";
  String rejectedTopic = String("$aws/provisioning-templates/") + template_name + "/provision/json/rejected";
  if (!mqtt.subscribe(acceptedTopic.c_str()) ||
      !mqtt.subscribe(rejectedTopic.c_str())) {
    out_error = "subscribe (provision topics) failed";
    mqtt.disconnect();
    return AWS_CLAIM_CONNECT_FAILED;
  }

  DynamicJsonDocument reg(4096);
  reg["certificateOwnershipToken"] = g_token;
  if (parameters.length() > 0) {
    DynamicJsonDocument params(1024);
    if (deserializeJson(params, parameters)) {
      out_error = "parameters: invalid JSON";
      mqtt.disconnect();
      return AWS_CLAIM_BAD_INPUT;
    }
    reg["parameters"] = params.as<JsonObject>();
  } else {
    // Default: SerialNumber = MAC12 (lowercase, no separators). Provisioning
    // templates that bind on $serialNumber will Just Work.
    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);
    char macStr[16];
    snprintf(macStr, sizeof(macStr), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    JsonObject p = reg["parameters"].to<JsonObject>();
    p["SerialNumber"] = macStr;
  }
  String regPayload;
  serializeJson(reg, regPayload);

  String regTopic = String("$aws/provisioning-templates/") + template_name + "/provision/json";
  g_phase = PHASE_WAIT_REGISTER;
  if (!mqtt.publish(regTopic.c_str(), regPayload.c_str())) {
    out_error = "publish (register thing) failed";
    mqtt.disconnect();
    return AWS_CLAIM_CONNECT_FAILED;
  }
  if (!waitForPhase(mqtt, PHASE_WAIT_REGISTER, 15000)) {
    out_error = "timeout waiting for register thing";
    mqtt.disconnect();
    return AWS_CLAIM_TIMEOUT;
  }
  if (g_phase == PHASE_DONE_ERR) {
    out_error = g_errorMsg;
    mqtt.disconnect();
    return AWS_CLAIM_REJECTED;
  }
  // g_phase == PHASE_DONE_OK

  mqtt.disconnect();

  // Persist permanent identity. awsIotProvision() also runs awsIotSetup()
  // internally, so the persistent module re-inits with the new cert.
  // root_ca passes through unchanged — empty means "use bundled".
  if (!awsIotProvision(endpoint, root_ca, g_newThing, g_newCert, g_newKey)) {
    out_error = "failed to persist permanent identity";
    return AWS_CLAIM_INTERNAL_ERROR;
  }
  out_thing_name = g_newThing;

  // Drop the new cert/key from module memory; aws_iot.cpp owns the live
  // copies now via its own Preferences load.
  g_token = "";
  g_newCert = "";
  g_newKey = "";
  g_newThing = "";
  g_currentTemplate = "";
  return AWS_CLAIM_OK;
}

const char* awsClaimResultName(AwsClaimResult r) {
  switch (r) {
    case AWS_CLAIM_OK:             return "ok";
    case AWS_CLAIM_BAD_INPUT:      return "bad_input";
    case AWS_CLAIM_NO_WIFI:        return "no_wifi";
    case AWS_CLAIM_NTP_FAILED:     return "ntp_failed";
    case AWS_CLAIM_CONNECT_FAILED: return "connect_failed";
    case AWS_CLAIM_TIMEOUT:        return "timeout";
    case AWS_CLAIM_REJECTED:       return "rejected";
    case AWS_CLAIM_INTERNAL_ERROR: return "internal_error";
  }
  return "unknown";
}

#endif  // ENABLE_AWS_IOT
