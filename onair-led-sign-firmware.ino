static const char* BOARD_NAME = "XIAO-ESP32-C6";
#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif
/*
================================================================================
 ESP32-C6 Setup Portal + API + OTA + Factory Reset
================================================================================

WHAT THIS DOES
--------------
- Captive portal for initial Wi-Fi provisioning (AP+STA).
- HTTP API to read status and toggle an output GPIO.
- OTA firmware upload via /update.
- mDNS advertising using the configured hostname.
- Factory reset via BOOT long-press for 5s.

DEFAULTS / BEHAVIOR
-------------------
- Setup AP SSID: "C6-SETUP-" + device MAC suffix.
- Setup AP password: generated once and stored; can be set to empty for open AP.
- Portal auto-scans for Wi-Fi networks on load; manual/hidden SSID supported.
- Hostname defaults to a normalized BOARD_NAME, still configurable.

REQUIRED LIBRARIES (as you installed)
------------------------------------
- ESPAsyncWebServer v3.9.4 by ESP32Async https://github.com/ESP32Async/ESPAsyncWebServer/releases/tag/v3.9.4
- Async TCP v3.4.10 by ESP32Async - Adruino IDE built-in installed via libraries manager
- ArduinoJson v7.4.2 by Benoit Blanchon - Adruino IDE built-in installed via libraries manager

================================================================================
*/

#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <ArduinoJson.h>

// ---------- Optional BLE provisioning (compile-time) ----------
#define ENABLE_BLE_PROV 0

#if defined(__has_include)
  #if __has_include(<WiFiProv.h>)
    #include <WiFiProv.h>
    #define HAS_WIFI_PROV 1
  #else
    #define HAS_WIFI_PROV 0
  #endif
  #if __has_include(<esp_wifi.h>)
    #include <esp_wifi.h>
    #define HAS_ESP_WIFI_H 1
  #else
    #define HAS_ESP_WIFI_H 0
  #endif
#else
  #define HAS_WIFI_PROV 0
  #define HAS_ESP_WIFI_H 0
#endif

#ifndef LED_BUILTIN
#define LED_BUILTIN 8   // Many ESP32-C6 boards use GPIO8; adjust if needed.
#endif

// ---------------- Pins / defaults ----------------
static const int PIN_BOOT = 9;     // BOOT commonly GPIO9
static int       PIN_OUT  = 6;     // configurable output pin (default GPIO6)
static int       PIN_LED  = LED_BUILTIN;

// LED activation polarity (some boards drive LED LOW to turn on)
static bool LED_ACTIVE_HIGH = true;  // configurable from portal

// ---------------- Output mode / breathing ----------------
enum OutputMode {
  MODE_OFF = 0,
  MODE_ON = 1,
  MODE_BREATHING = 2
};

static int outputMode = MODE_OFF;
// Defaults: 3000ms, 5% min, 100% max. API range: 500-10000ms, 1-99 min, 1-100 max.
static uint32_t breathPeriodMs = 3000; // total period
static uint8_t breathMinPct = 5;       // 1..99
static uint8_t breathMaxPct = 100;     // 1..100
static uint32_t breathPhaseStart = 0;

static const uint16_t PWM_FREQ_HZ = 1000;
static const uint8_t PWM_RES_BITS = 10;
static const uint16_t PWM_MAX = (1 << PWM_RES_BITS) - 1;
static bool pwmAttached = false;
static int pwmPin = -1;

// ---------------- Captive portal / Web server ----------------
static DNSServer dnsServer;
static const byte DNS_PORT = 53;
static AsyncWebServer server(80);

// ---------------- Storage ----------------
static Preferences prefs;

// ---------------- State ----------------
static bool portalMode = false;

// Setup AP timeout (auto reboot if still in setup mode)
static const uint32_t SETUP_AP_TIMEOUT_MS = 10UL * 60UL * 1000UL; // 10 minutes
static uint32_t portalStartedMs = 0;

// ---------------- Factory reset long-press ----------------
static bool bootPressed = false;
static uint32_t bootPressStart = 0;
static const uint32_t RESET_HOLD_MS = 5000;
static bool resetFeedbackActive = false;
static int savedOutputMode = MODE_OFF;

// ---------------- Setup AP ----------------
static String apSsid;
static String apPassStr; // WPA2/WPA3 password (>= 8 chars)
static IPAddress apIP(192, 168, 4, 1);
static IPAddress apGW(192, 168, 4, 1);
static IPAddress apSN(255, 255, 255, 0);

// ---------------- WiFi scan (non-blocking) ----------------
static volatile bool scanRequested = false;
static int lastScanCount = -2; // -2 means "never started"
static uint32_t lastScanStartedMs = 0;
static const uint32_t SCAN_MAX_WAIT_MS = 6000;

// ---------------- Delayed reboot ----------------

// ---------------- Helpers ----------------

void ledWrite(bool on) {
  bool level = LED_ACTIVE_HIGH ? on : !on;
  digitalWrite(PIN_LED, level ? HIGH : LOW);
}

void ledSlowBlinkTick() {
  static uint32_t last = 0;
  static bool state = false;
  if (millis() - last >= 500) { // slow blink
    last = millis();
    state = !state;
    ledWrite(state);
  }
}

void outputWriteDigital(bool on) {
  if (PIN_OUT == PIN_LED) { ledWrite(on); return; }
  digitalWrite(PIN_OUT, on ? HIGH : LOW);
}

void ensurePwmAttached() {
  if (pwmAttached && pwmPin == PIN_OUT) return;
  if (pwmAttached) ledcDetach(pwmPin);
  ledcAttach(PIN_OUT, PWM_FREQ_HZ, PWM_RES_BITS);
  pwmAttached = true;
  pwmPin = PIN_OUT;
}

void outputSetLevelPct(uint8_t pct) {
  if (pct > 100) pct = 100;
  if (pwmAttached) {
    uint32_t duty = (uint32_t)pct * PWM_MAX / 100;
    if (PIN_OUT == PIN_LED && !LED_ACTIVE_HIGH) {
      duty = PWM_MAX - duty;
    }
    ledcWrite(PIN_OUT, duty);
    return;
  }
  outputWriteDigital(pct > 0);
}

void outputWrite(bool on) {
  outputSetLevelPct(on ? 100 : 0);
}

void setOutputMode(int mode) {
  if (mode < MODE_OFF || mode > MODE_BREATHING) mode = MODE_OFF;
  outputMode = mode;
  if (outputMode == MODE_BREATHING) {
    ensurePwmAttached();
    breathPhaseStart = millis();
  }
  if (outputMode == MODE_ON) {
    outputWrite(true);
  } else if (outputMode == MODE_OFF) {
    outputWrite(false);
  }
}

void breathingTick() {
  if (outputMode != MODE_BREATHING) return;
  uint32_t period = breathPeriodMs ? breathPeriodMs : 3000;
  uint32_t half = period / 2;
  uint32_t elapsed = (millis() - breathPhaseStart) % period;
  uint32_t span = (breathMaxPct > breathMinPct) ? (breathMaxPct - breathMinPct) : 0;
  uint8_t pct;
  if (half == 0) {
    pct = breathMaxPct;
  } else if (elapsed < half) {
    pct = breathMinPct + (uint8_t)((span * elapsed) / half);
  } else {
    uint32_t down = elapsed - half;
    pct = breathMaxPct - (uint8_t)((span * down) / half);
  }
  outputSetLevelPct(pct);
}

String htmlEscape(const String& in) {
  String s = in;
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}

String loadString(const char* key, const char* def = "") { return prefs.getString(key, def); }
int    loadInt(const char* key, int def)                 { return prefs.getInt(key, def); }
bool   loadBool(const char* key, bool def)               { return prefs.getBool(key, def); }

String defaultHostName() {
  String name = String(BOARD_NAME);
  name.toLowerCase();
  String out;
  for (size_t i = 0; i < name.length(); i++) {
    char c = name[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out += c;
    } else if (c == '-' || c == '_') {
      out += c;
    } else {
      out += '-';
    }
  }
  while (out.length() && out[0] == '-') out.remove(0, 1);
  while (out.length() && out[out.length() - 1] == '-') out.remove(out.length() - 1, 1);
  if (out.length() == 0) out = "esp32c6";
  if (out.length() > 32) out.remove(32);
  return out;
}

void saveConfig(const String& ssid, const String& pass, const String& host, int outPin, bool ledah, bool usebl,
                const String& authUser, const String& authPass, const String& apPass) {
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("host", host);
  prefs.putInt("out", outPin);
  prefs.putBool("usebl", usebl);
  prefs.putBool("ledah", ledah);
  prefs.putString("auth_user", authUser);
  prefs.putString("auth_pass", authPass);
  prefs.putString("ap_pass", apPass);
}

void clearConfig() { prefs.clear(); }

// Built-in small wordlist for easy-to-type passwords.
static const char* WORDS[] = {
  "mint","plum","sky","river","stone","laser","panda","tiger","maple","cider","pearl","ember",
  "ocean","olive","sun","cloud","storm","delta","orbit","pixel","cobalt","coral","saffron","koi",
  "luna","nova","terra","flora","canyon","ridge","breeze","zephyr","aurora","comet","neon","iris",
  "eagle","fox","otter","koala","gecko","sparrow","falcon","whale","mango","cocoa","honey","latte"
};

String loadAuthUser() { return loadString("auth_user", "admin"); }
String loadAuthPass() { return loadString("auth_pass", "esp32c6"); }
String loadApiToken() { return loadString("api_token", ""); }

String makeApiToken() {
  uint64_t mac = ESP.getEfuseMac();
  uint32_t r1 = esp_random();
  uint32_t r2 = esp_random();
  char b[33];
  snprintf(b, sizeof(b), "%012llX%08X%08X",
           (unsigned long long)(mac & 0xFFFFFFFFFFFFULL),
           (unsigned)r1, (unsigned)r2);
  return String(b);
}

bool hasValidApiToken(AsyncWebServerRequest *request) {
  String token = loadApiToken();
  if (token.length() == 0) return false;
  if (request->hasHeader("X-API-Token")) {
    if (request->getHeader("X-API-Token")->value() == token) return true;
  }
  if (request->hasHeader("Authorization")) {
    String auth = request->getHeader("Authorization")->value();
    const String prefix = "Bearer ";
    if (auth.startsWith(prefix) && auth.substring(prefix.length()) == token) return true;
  }
  if (request->hasParam("token")) {
    if (request->getParam("token")->value() == token) return true;
  }
  return false;
}

bool checkBasicAuth(AsyncWebServerRequest *request) {
  String user = loadAuthUser();
  String pass = loadAuthPass();
  return request->authenticate(user.c_str(), pass.c_str());
}

bool ensureBasicAuth(AsyncWebServerRequest *request) {
  if (checkBasicAuth(request)) return true;
  request->requestAuthentication();
  return false;
}

bool ensureApiAuth(AsyncWebServerRequest *request) {
  if (hasValidApiToken(request)) return true;
  if (checkBasicAuth(request)) return true;
  request->requestAuthentication();
  return false;
}

String makeFriendlyApPassword() {
  // Random, WPA2/WPA3-compliant (>=8 chars), easy-to-type: "<word>-<word>-NN"
  const size_t W = sizeof(WORDS) / sizeof(WORDS[0]);
  uint32_t r1 = esp_random();
  uint32_t r2 = esp_random();
  uint32_t r3 = esp_random();
  const char* w1 = WORDS[r1 % W];
  const char* w2 = WORDS[r2 % W];
  int num = (int)(r3 % 90) + 10; // 10..99
  String pass = String(w1) + "-" + String(w2) + "-" + String(num);
  if (pass.length() < 8) pass += "-99";
  return pass;
}

String makeChipIdHex4() {
  uint64_t mac = ESP.getEfuseMac();
  uint16_t x = (uint16_t)(mac & 0xFFFF);
  char b[5];
  snprintf(b, sizeof(b), "%04X", x);
  return String(b);
}

String makeChipIdHex6() {
  // Use 24 bits of the eFuse MAC for a more collision-resistant suffix
  // (Still short enough to keep SSIDs readable).
  uint64_t mac = ESP.getEfuseMac();
  uint32_t x = (uint32_t)(mac & 0xFFFFFF);
  char b[7];
  snprintf(b, sizeof(b), "%06X", x);
  return String(b);
}

String makeMacHex12() {
  // Use the device's base (eFuse) MAC address as a stable unique suffix.
  // Format: 12 hex chars, no separators (e.g. A1B2C3D4E5F6)
  // SSID length stays well under 32 chars: "C6-SETUP-" (8) + 12 = 20.
  uint64_t mac = ESP.getEfuseMac(); // 48-bit MAC in the low bits
  uint8_t b0 = (mac >> 40) & 0xFF;
  uint8_t b1 = (mac >> 32) & 0xFF;
  uint8_t b2 = (mac >> 24) & 0xFF;
  uint8_t b3 = (mac >> 16) & 0xFF;
  uint8_t b4 = (mac >> 8)  & 0xFF;
  uint8_t b5 = (mac >> 0)  & 0xFF;
  char out[13];
  snprintf(out, sizeof(out), "%02X%02X%02X%02X%02X%02X", b0, b1, b2, b3, b4, b5);
  return String(out);
}

String wifiQrPayload(const String& ssid, const String& pass) {
  if (pass.length() == 0) {
    return "WIFI:T:nopass;S:" + ssid + ";H:false;;";
  }
  return "WIFI:T:WPA;S:" + ssid + ";P:" + pass + ";H:false;;";
}

// URL-encode a string for use in query parameters (RFC 3986 unreserved: A-Z a-z 0-9 - _ . ~)
String urlEncode(const String& s) {
  String out;
  out.reserve(s.length() * 3);
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    const uint8_t c = (uint8_t)s[i];
    const bool unreserved =
      (c >= 'A' && c <= 'Z') ||
      (c >= 'a' && c <= 'z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out += (char)c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

String wifiQrQuickChartUrl(const String& ssid, const String& pass, int sizePx) {
  const String payload = wifiQrPayload(ssid, pass);
  return "https://quickchart.io/qr?text=" + urlEncode(payload) + "&size=" + String(sizePx);
}


// ---------- Captive portal: common OS endpoints ----------
bool isCaptiveProbe(AsyncWebServerRequest *request) {
  String u = request->url();
  u.toLowerCase();
  // Apple
  if (u == "/hotspot-detect.html" || u == "/library/test/success.html" || u == "/success.html") return true;
  // Android
  if (u == "/generate_204" || u == "/gen_204") return true;
  // Windows
  if (u == "/ncsi.txt" || u == "/connecttest.txt") return true;
  return false;
}

void captiveRedirect(AsyncWebServerRequest *request) {
  request->redirect(String("http://") + apIP.toString() + "/");
}

// ---------------- UI (clean + minimal) ----------------

String pageShell(const String& title, const String& body, const String& script = "", bool titleInline = false) {
  String h;
  h.reserve(9000);
  h += "<!doctype html><html><head><meta charset='utf-8'/>"
       "<meta name='viewport' content='width=device-width, initial-scale=1'/>"
       "<title>" + htmlEscape(title) + "</title>"
       "<style>"
       "*{box-sizing:border-box}"
       ":root{--bg:#0b1220;--card:#111a2e;--txt:#eaf0ff;--mut:#a9b7d6;--acc:#5eead4;--bad:#fb7185;}"
       "body{margin:0;font-family:ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial;"
       "background:radial-gradient(1200px 600px at 20% 0%, #1b2a52 0%, var(--bg) 60%);color:var(--txt);}"
       ".wrap{max-width:720px;margin:0 auto;padding:24px;}"
       ".card{background:rgba(17,26,46,.92);border:1px solid rgba(94,234,212,.18);"
       "box-shadow:0 20px 60px rgba(0,0,0,.35);border-radius:18px;padding:20px;backdrop-filter:blur(6px);}"
       "h1{font-size:22px;margin:0 0 8px;letter-spacing:.2px;}"
       "p{margin:10px 0;color:var(--mut);line-height:1.5}"
       ".row{display:grid;grid-template-columns:1fr;gap:12px;margin-top:14px}"
       "label{font-size:13px;color:var(--mut)}"
       "input,select{width:100%;padding:12px 12px;border-radius:12px;border:1px solid rgba(169,183,214,.22);"
       "background:#0b1326;color:var(--txt);outline:none}"
       "input:focus,select:focus{border-color:rgba(94,234,212,.65);box-shadow:0 0 0 4px rgba(94,234,212,.12)}"
       ".btns{display:flex;gap:10px;flex-wrap:wrap;margin-top:14px;margin-bottom:10px}"
       "button{padding:10px 12px;border-radius:12px;border:1px solid rgba(94,234,212,.35);"
       "background:linear-gradient(180deg, rgba(94,234,212,.18), rgba(94,234,212,.06));"
       "color:var(--txt);cursor:pointer;font-weight:700;font-size:13px}"
       "button:disabled{opacity:.55;cursor:not-allowed}"
       "button.active:disabled{opacity:1}"
       "button.secondary{border-color:rgba(169,183,214,.25);background:rgba(169,183,214,.06);color:var(--mut)}"
       "button.active{border-color:rgba(94,234,212,.75);background:linear-gradient(180deg, rgba(94,234,212,.45), rgba(94,234,212,.12));"
       "color:var(--txt);box-shadow:0 0 0 3px rgba(94,234,212,.12) inset}"
       "@keyframes pulse{0%{transform:translateY(0);box-shadow:0 0 0 0 rgba(94,234,212,.25)}"
       "50%{transform:translateY(-1px);box-shadow:0 0 0 6px rgba(94,234,212,.08)}"
       "100%{transform:translateY(0);box-shadow:0 0 0 0 rgba(94,234,212,.25)}}"
       "button.busy{animation:pulse .8s ease-in-out infinite}"
       ".spinner{width:18px;height:18px;border-radius:50%;border:2px solid rgba(169,183,214,.35);"
       "border-top-color:rgba(94,234,212,.9);animation:spin 1s linear infinite;display:inline-block}"
       "@keyframes spin{to{transform:rotate(360deg)}}"
       ".hint{font-size:12px;color:var(--mut)}"
       ".pill{display:inline-flex;align-items:center;gap:8px;font-size:12px;color:var(--mut)}"
       ".head{display:flex;align-items:center;gap:10px;flex-wrap:wrap}"
       ".pill a{display:inline-flex;align-items:center;gap:8px;padding:10px 12px;border-radius:12px;"
       "border:1px solid rgba(169,183,214,.25);background:rgba(169,183,214,.06);color:var(--mut);text-decoration:none;font-weight:700}"
       ".inline-title{font-size:16px;font-weight:700;color:var(--txt);margin:0}"
       ".section{margin-top:18px}"
       ".ok{color:var(--acc)}.bad{color:var(--bad)}"
       ".check{display:flex;gap:8px;align-items:center;user-select:none}"
       ".check input{width:auto}"
       ".grid2{display:grid;grid-template-columns:1fr;gap:12px}"
       "@media(min-width:720px){.grid2{grid-template-columns:1fr 1fr}}"
       "code{background:rgba(169,183,214,.08);padding:2px 6px;border-radius:8px;border:1px solid rgba(169,183,214,.14)}"
       "</style></head><body><div class='wrap'><div class='card'>"
       + (titleInline && title.length()
            ? "<div class='head'><div class='pill'><a href='/'>ESP32-C6 • Setup</a></div><div class='inline-title'>" + htmlEscape(title) + "</div></div>"
            : "<div class='pill'><a href='/'>ESP32-C6 • Setup</a></div>")
       + ((!titleInline && title.length()) ? "<h1>" + htmlEscape(title) + "</h1>" : "")
       + body +
       "</div></div>"
       "<script>" + script + "</script>"
       "</body></html>";
  return h;
}

String setupPage() {
  String savedHost = loadString("host", defaultHostName().c_str());
  String savedSsid = loadString("ssid", "");
  int savedOut = loadInt("out", 6);
  bool savedUseBL = loadBool("usebl", false);
  bool savedLedAH = loadBool("ledah", true);
  String savedAuthUser = loadAuthUser();
  String savedAuthPass = loadAuthPass();
  String savedApPass = loadString("ap_pass", "");
  int savedOutEff = savedUseBL ? PIN_LED : savedOut;
  const int commonPins[] = {6, 7, 8, 9, 10};
  bool outIsCommon = false;
  for (size_t i = 0; i < sizeof(commonPins) / sizeof(commonPins[0]); i++) {
    if (savedOutEff == commonPins[i]) { outIsCommon = true; break; }
  }
  String outSel = outIsCommon ? String(savedOutEff) : "__custom__";
  String outCustom = outIsCommon ? "" : String(savedOutEff);

  String body;
  body += "<p>Choose your Wi‑Fi and device name, then tap <b>Save & Connect</b>.</p>";
  body += "<div class='row'>";

  body += "<div><label>Wi‑Fi network</label>"
          "<select id='ssidSel'><option value=''>Scanning…</option></select>"
          "<div id='manualWrap' style='display:none;margin-top:8px'><input id='ssid' placeholder='Type SSID' value='" + htmlEscape(savedSsid) + "' autocomplete='off' maxlength='32'/></div>"
          "<div class='hint'>Pick a network or choose manual for hidden SSIDs.</div></div>";
  body += "<div><label>Wi‑Fi password</label><input id='pass' type='password' autocomplete='off' maxlength='63'/></div>";

  body += "<div class='grid2'>";
  body += "<div><label>Device name</label><input id='host' value='" + htmlEscape(savedHost) + "' autocomplete='off' maxlength='32'/></div>";
  body += "<div><label>Output GPIO</label>"
          "<select id='out_sel'>"
          "<option value='6'>GPIO 6</option>"
          "<option value='7'>GPIO 7</option>"
          "<option value='8'>GPIO 8 (LED)</option>"
          "<option value='9'>GPIO 9</option>"
          "<option value='10'>GPIO 10</option>"
          "<option value='__custom__'>Custom…</option>"
          "</select>"
          "<div class='hint'>Breathing uses PWM-capable GPIOs.</div></div>";
  body += "</div>";
  body += "<div class='row' id='outCustomWrap' style='display:none'>"
          "<label>Custom GPIO</label><input id='out_custom' type='number' min='0' max='48' value='" + htmlEscape(outCustom) + "'/>"
          "</div>";

  body += "<details><summary class='hint'>Advanced</summary>";
  body += "<div class='grid2'>";
  body += "<div><label>Onboard LED active level</label>"
          "<select id='ledah'>"
          "<option value='1'" + String(savedLedAH ? " selected" : "") + ">Active HIGH (ON = HIGH)</option>"
          "<option value='0'" + String(!savedLedAH ? " selected" : "") + ">Active LOW (ON = LOW)</option>"
          "</select></div>";
  body += "<div><label>Use built‑in LED</label>"
          "<label class='check'><input id='usebl' type='checkbox'" + String(savedUseBL ? " checked" : "") + "/>Use built‑in LED as output (GPIO " + String(PIN_LED) + ")</label></div>";
  body += "</div>";
  body += "<div class='grid2'>";
  body += "<div><label>Admin user</label><input id='auth_user' value='" + htmlEscape(savedAuthUser) + "' autocomplete='off' maxlength='32'/></div>";
  body += "<div><label>Admin password</label><input id='auth_pass' type='password' value='" + htmlEscape(savedAuthPass) + "' autocomplete='off' maxlength='64'/></div>";
  body += "</div>";
  body += "<div class='grid2'>";
  body += "<div><label>Setup AP password (optional)</label><input id='ap_pass' type='password' value='" + htmlEscape(savedApPass) + "' autocomplete='off' maxlength='63' placeholder='Leave empty for open AP'/></div>";
  body += "<div class='hint'>Leave empty for an open setup AP. WPA2 requires 8+ characters.</div>";
  body += "</div>";
  body += "</details>";

  body += "<div class='btns'>"
          "<button id='btnSave' onclick='saveCfg()'>Save & Connect</button>"
          "</div>";

  body += "<p class='hint' id='msg'></p>";
  body += "</div>";

  // Non-blocking scan flow:
  // - /scan -> {scanning:true} when scan started
  // - /scan -> {ssids:[...]} when scan complete
  String script;
  script += "const DEFAULT_HOST='" + htmlEscape(savedHost) + "';"
            "const OUT_SEL_DEFAULT='" + htmlEscape(outSel) + "';"
            "function esc(s){return String(s).replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;').replaceAll('\"','&quot;').replaceAll(\"'\",'&#39;');}"
            "function setMsg(t,bad){const el=document.getElementById('msg'); el.textContent=t; el.className='hint '+(bad?'bad':'');}"
            "function toggleCustomOut(){const sel=document.getElementById('out_sel'); const wrap=document.getElementById('outCustomWrap'); if(!sel||!wrap) return; wrap.style.display=(sel.value==='__custom__')?'block':'none';}"
            "async function rescan(){"
            "  setMsg('Scanning…');"
            "  let tries=0;"
            "  while(tries<12){"
            "    const r=await fetch('/scan');"
            "    const j=await r.json();"
            "    if(j.scanning){tries++; await new Promise(res=>setTimeout(res,450)); continue;}"
            "    const list=document.getElementById('ssidSel');"
            "    const input=document.getElementById('ssid');"
            "    list.innerHTML='';"
            "    if(!j.ssids||!j.ssids.length){"
            "      list.innerHTML='<option value=\"__manual__\">Manual / hidden SSID…</option>';"
            "      setMsg('No networks found. Enter SSID manually.');"
            "      toggleManual();"
            "      return;"
            "    }"
            "    list.innerHTML='<option value=\"\">Select…</option>' + j.ssids.map(s=>`<option value=\"${esc(s)}\">${esc(s)}</option>`).join('') + '<option value=\"__manual__\">Manual / hidden SSID…</option>';"
            "    setMsg('');"
            "    toggleManual();"
            "    return;"
            "  }"
            "  setMsg('Scan taking longer than expected. Try again.', true);"
            "}"
            "function toggleManual(){const sel=document.getElementById('ssidSel'); const wrap=document.getElementById('manualWrap'); if(!sel||!wrap) return; wrap.style.display=(sel.value==='__manual__')?'block':'none';}"
            "function setActiveBtn(id){const b=document.getElementById(id); if(!b) return; b.classList.add('active');}"
            "async function saveCfg(){"
            "  setActiveBtn('btnSave');"
            "  const ssSel=document.getElementById('ssidSel').value.trim();"
            "  const ssMan=document.getElementById('ssid').value.trim();"
            "  const ssid=(ssSel==='__manual__'||!ssSel.length)?ssMan:ssSel;"
            "  const pass=document.getElementById('pass').value;"
            "  const host=document.getElementById('host').value.trim()||DEFAULT_HOST;"
            "  const usebl=document.getElementById('usebl').checked;"
            "  const outSel=document.getElementById('out_sel').value;"
            "  const outCustom=document.getElementById('out_custom').value;"
            "  const out=(usebl ? -1 : (outSel==='__custom__'?parseInt(outCustom||'6',10):parseInt(outSel,10)));"
            "  const ledah=document.getElementById('ledah').value==='1';"
            "  const auth_user=document.getElementById('auth_user').value.trim()||'admin';"
            "  const auth_pass=document.getElementById('auth_pass').value||'esp32c6';"
            "  const ap_pass=document.getElementById('ap_pass').value;"
            "  if(!ssid){setMsg('Please choose or type an SSID.', true); return;}"
            "  if(pass.length && pass.length < 8){setMsg('Wi-Fi password must be at least 8 characters.', true); return;}"
            "  if(ap_pass.length && ap_pass.length < 8){setMsg('AP password must be at least 8 characters.', true); return;}"
            "  setMsg('Saving…');"
            "  const r=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,pass,host,out,ledah,usebl,auth_user,auth_pass,ap_pass})});"
            "  const j=await r.json();"
            "  if(!j.ok){setMsg(j.error||'Save failed.', true); return;}"
            "  setMsg('Saved. Rebooting…');"
            "  setTimeout(()=>{location.href='/';},1200);"
            "};"
            "function syncOut(){const sel=document.getElementById('out_sel'); const custom=document.getElementById('out_custom'); const useEl=document.getElementById('usebl'); if(sel&&useEl){sel.disabled=useEl.checked;} if(custom&&useEl){custom.disabled=useEl.checked;}}"
            "document.addEventListener('DOMContentLoaded',()=>{const useEl=document.getElementById('usebl'); if(useEl){useEl.addEventListener('change',syncOut);} const outSel=document.getElementById('out_sel'); if(outSel){outSel.addEventListener('change',toggleCustomOut); outSel.value=OUT_SEL_DEFAULT;} const ssSel=document.getElementById('ssidSel'); if(ssSel){ssSel.addEventListener('change',toggleManual);} toggleCustomOut(); syncOut(); toggleManual(); rescan();});";
  return pageShell("Wi‑Fi Setup", body, script);
}

String connectedPage() {
  String host = loadString("host", defaultHostName().c_str());
  String ip = WiFi.localIP().toString();
  String token = loadApiToken();

  String body;
  body += "<p><span class='ok'>Connected</span> • IP <b>" + htmlEscape(ip) + "</b> • mDNS <b>" + htmlEscape(host) + ".local</b></p>";
  body += "<p class='hint'>FW: <code>" + htmlEscape(FW_VERSION) + "</code></p>";
  body += "<div class='btns' style='margin-top:16px'>"
          "<button id='btnOn' class='secondary' onclick='toggle(1)'>ON</button>"
          "<button id='btnOff' class='secondary' onclick='toggle(0)'>OFF</button>"
          "<button id='btnBreath' class='secondary' onclick='setBreathing()'>BREATHING</button>"
          "<button id='btnOta' class='secondary' onclick='openOta()'>OTA</button>"
          "</div>";
  body += "<details class='section'><summary class='hint'>Breathing settings</summary>";
  body += "<div class='grid2'>";
  body += "<div><label>Breathing period (ms)</label><input id='br_period' type='number' min='500' max='10000' value='" + String(breathPeriodMs) + "'/></div>";
  body += "<div><label>Breathing min (%)</label><input id='br_min' type='number' min='1' max='99' value='" + String(breathMinPct) + "'/></div>";
  body += "<div><label>Breathing max (%)</label><input id='br_max' type='number' min='1' max='100' value='" + String(breathMaxPct) + "'/></div>";
  body += "</div>";
  body += "<div class='btns'><button id='btnBreathApply' class='secondary' onclick='setBreathing()'>Apply breathing settings</button></div>";
  body += "</details>";
  body += "<details class='section'><summary class='hint'>API access</summary>";
  body += "<p>API: <code>/api/status</code>, <code>/api/set?state=1</code>, <code>/api/config</code>, <code>/api/reboot</code> • OTA: <code>/update</code></p>";
  body += "<p>Breathing: <code>/api/mode?mode=breathing&period_ms=3000&min_pct=5&max_pct=100</code></p>";
  body += "<p>API token: <code id='tok'>" + htmlEscape(token) + "</code> <button id='btnCopy' class='secondary' onclick='copyToken()'>Copy</button></p>";
  body += "</details>";
  body += "<p class='hint' id='msg'></p>";

  String script;
  script += "let activeMode=null;"
            "function setMsg(t,bad){const el=document.getElementById('msg'); if(!el) return; el.textContent=t; el.className='hint '+(bad?'bad':'');}"
            "function setActiveMode(mode){"
            "  const on=document.getElementById('btnOn');"
            "  const off=document.getElementById('btnOff');"
            "  const br=document.getElementById('btnBreath');"
            "  if(on) on.classList.remove('active');"
            "  if(off) off.classList.remove('active');"
            "  if(br) br.classList.remove('active');"
            "  if(mode==='on' && on) on.classList.add('active');"
            "  if(mode==='off' && off) off.classList.add('active');"
            "  if(mode==='breathing' && br) br.classList.add('active');"
            "  activeMode = mode;"
            "}"
            "function fetchWithTimeout(url, opts, ms){"
            "  const ctrl=new AbortController();"
            "  const t=setTimeout(()=>ctrl.abort(), ms||1600);"
            "  const o=Object.assign({}, opts||{}, {signal: ctrl.signal});"
            "  return fetch(url, o).finally(()=>clearTimeout(t));"
            "}"
            "function setBusyBtn(id,on){const b=document.getElementById(id); if(b) b.classList.toggle('busy', !!on);}"
            "function setBusy(ms){"
            "  const btns=document.querySelectorAll('button');"
            "  btns.forEach(b=>{b.disabled=true;});"
            "  setTimeout(()=>{btns.forEach(b=>{b.disabled=false;});}, ms||600);"
            "}"
            "async function toggle(s){"
            "  const prev=activeMode;"
            "  setActiveMode(s ? 'on' : 'off');"
            "  setMsg(s? 'Turning ON…':'Turning OFF…');"
            "  setBusyBtn(s ? 'btnOn' : 'btnOff', true);"
            "  setBusy(500);"
            "  try{"
            "    const r=await fetchWithTimeout('/api/set?state='+s, {}, 2000);"
            "    const j=await r.json();"
            "    if(j.ok){setMsg('Output is now '+(j.state?'ON':'OFF'));}"
            "    else{setMsg(j.error||'Failed', true); if(prev) setActiveMode(prev);}"
            "  }catch(e){"
            "    setMsg('Device busy or rebooting. Try again in a few seconds.', true);"
            "    if(prev) setActiveMode(prev);"
            "  }"
            "  setBusyBtn('btnOn', false);"
            "  setBusyBtn('btnOff', false);"
            "}"
            "function copyToken(){"
            "  const b=document.getElementById('btnCopy');"
            "  if(b){b.classList.add('active'); b.classList.add('busy');}"
            "  const done=(ok)=>{setMsg(ok?'Token copied':'Copy failed', !ok); if(b){setTimeout(()=>{b.classList.remove('busy'); b.classList.remove('active');},900);}};"
            "  const t=document.getElementById('tok').textContent;"
            "  if(navigator.clipboard&&navigator.clipboard.writeText){"
            "    navigator.clipboard.writeText(t).then(()=>done(true)).catch(()=>done(false));"
            "    return;"
            "  }"
            "  try{"
            "    const ta=document.createElement('textarea'); ta.value=t; document.body.appendChild(ta); ta.select();"
            "    const ok=document.execCommand('copy'); document.body.removeChild(ta);"
            "    done(!!ok);"
            "  }catch(e){done(false);}"
            "}"
            "async function setBreathing(){"
            "  const prev=activeMode;"
            "  setActiveMode('breathing');"
            "  const p=parseInt(document.getElementById('br_period').value||'3000',10);"
            "  const mn=parseInt(document.getElementById('br_min').value||'5',10);"
            "  const mx=parseInt(document.getElementById('br_max').value||'100',10);"
            "  setMsg('Applying breathing…');"
            "  setBusyBtn('btnBreath', true);"
            "  setBusyBtn('btnBreathApply', true);"
            "  setBusy(700);"
            "  try{"
            "    const r=await fetchWithTimeout(`/api/mode?mode=breathing&period_ms=${p}&min_pct=${mn}&max_pct=${mx}`, {}, 2000);"
            "    const j=await r.json();"
            "    if(j.ok){setMsg('Breathing enabled');}"
            "    else{setMsg(j.error||'Failed', true); if(prev) setActiveMode(prev);}"
            "  }catch(e){"
            "    setMsg('Device busy or rebooting. Try again in a few seconds.', true);"
            "    if(prev) setActiveMode(prev);"
            "  }"
            "  setBusyBtn('btnBreath', false);"
            "  setBusyBtn('btnBreathApply', false);"
            "}"
            "function openOta(){const b=document.getElementById('btnOta'); if(b) b.classList.add('active'); setBusyBtn('btnOta', true); setMsg('Opening OTA…'); window.location.href='/update';}"
            "async function initStateFromDevice(){"
            "  try{"
            "    const r=await fetchWithTimeout('/api/status', {}, 1600);"
            "    const j=await r.json();"
            "    if(j&&j.ok&&j.output_mode){setActiveMode(j.output_mode); setMsg(''); return true;}"
            "  }catch(e){}"
            "  setMsg('Device busy or rebooting. Try again in a few seconds.', true);"
            "  return false;"
            "}"
            "document.addEventListener('DOMContentLoaded',()=>{"
            "  setMsg('Connecting to device…');"
            "  let tries=0;"
            "  const tick=async()=>{"
            "    tries++;"
            "    const ok=await initStateFromDevice();"
            "    if(!ok && tries<6){setTimeout(tick, 2000);} else if(ok){setMsg('');}"
            "  };"
            "  tick();"
            "});";
  return pageShell("", body, script);
}

// ---------------- Factory reset ----------------

void doFactoryReset() {
  Serial.println("\nFACTORY RESET: clearing saved config…");
  ledWrite(true);
  clearConfig();
  delay(300);
  ESP.restart();
}

void scheduleReboot(uint32_t delayMs = 200) {
  Serial.println("Reboot requested.");
  delay(delayMs);
  ESP.restart();
}

void handleResetLongPress() {
  bool pressed = (digitalRead(PIN_BOOT) == LOW); // pull-up
  if (pressed && !bootPressed) {
    bootPressed = true;
    bootPressStart = millis();
    resetFeedbackActive = true;
    savedOutputMode = outputMode;
  }
  if (!pressed && bootPressed) {
    bootPressed = false;
    ledWrite(false);
    if (resetFeedbackActive) {
      resetFeedbackActive = false;
      setOutputMode(savedOutputMode);
    }
    return;
  }
  if (bootPressed) {
    uint32_t held = millis() - bootPressStart;
    if (held < RESET_HOLD_MS) {
      ledSlowBlinkTick();
    } else {
      ledWrite(true);
      delay(150);
      doFactoryReset();
    }
  }
}

void checkBootTimeFactoryReset() {
  if (digitalRead(PIN_BOOT) == LOW) {
    Serial.println("BOOT held at startup. Hold for 5s to factory reset…");
    bootPressed = true;
    bootPressStart = millis();
    while (digitalRead(PIN_BOOT) == LOW) {
      handleResetLongPress(); // will reset at 5s
      delay(10);
    }
    bootPressed = false;
    ledWrite(false);
    Serial.println("BOOT released before reset. Continuing boot.");
  }
}

// ---------------- WiFi / AP ----------------

void stopPortalServices() {
  if (!portalMode) return;
  portalMode = false;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  Serial.println("Setup AP stopped.");
}

bool connectWifiWithSaved(uint32_t timeoutMs = 12000) {
  String ssid = loadString("ssid", "");
  String pass = loadString("pass", "");
  if (ssid.length() == 0) return false;

  String host = loadString("host", defaultHostName().c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(host.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());

  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    handleResetLongPress();
    delay(50);
  }
  return (WiFi.status() == WL_CONNECTED);
}

void maybeEnableWpa3TransitionMode() {
#if HAS_ESP_WIFI_H
  #if defined(WIFI_AUTH_WPA2_WPA3_PSK)
    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_AP, &cfg) == ESP_OK) {
      // Only switch if currently WPA2 or weaker
      cfg.ap.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
      esp_err_t e = esp_wifi_set_config(WIFI_IF_AP, &cfg);
      if (e == ESP_OK) {
        Serial.println("AP auth: WPA2/WPA3 transition (WPA3-ready)");
      } else {
        Serial.println("AP auth: WPA2 (could not enable WPA2/WPA3 transition)");
      }
    }
  #else
    // Core doesn't expose WIFI_AUTH_WPA2_WPA3_PSK yet
  #endif
#endif
}

// Start async scan if not already in progress
void ensureScanStarted() {
  int st = WiFi.scanComplete();
  if (st == WIFI_SCAN_RUNNING) return;

  // If previous scan complete, keep results until consumed
  if (st >= 0) return;

  // Start a new async scan
  WiFi.scanDelete();
  WiFi.scanNetworks(true /*async*/, true /*show_hidden*/);
  lastScanStartedMs = millis();
}

void startCaptivePortal() {
  portalMode = true;
  portalStartedMs = millis();

  // AP+STA simultaneously: stable for captive portal and scanning without mode flips
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apGW, apSN);

  // Stable softAP API (no authmode overload)
  if (apPassStr.length() >= 8) {
    WiFi.softAP(apSsid.c_str(), apPassStr.c_str(), 1 /*channel*/, 0 /*hidden*/, 4 /*maxconn*/);
  } else {
    WiFi.softAP(apSsid.c_str());
  }

  // Optionally upgrade AP auth to WPA2/WPA3 transition if supported
  maybeEnableWpa3TransitionMode();

  dnsServer.start(DNS_PORT, "*", apIP);

  Serial.println();
  Serial.println("== Setup Portal ==");
  Serial.print("AP SSID: "); Serial.println(apSsid);
  Serial.println(String("AP SSID (full): ") + apSsid);
  Serial.print("AP PASS: "); Serial.println(apPassStr.length() ? apPassStr : String("(open)"));
  Serial.print("AP IP:   "); Serial.println(WiFi.softAPIP());
  if (apPassStr.length()) {
    Serial.println("WiFi QR payload (copy-paste into QR generator):");
    Serial.println(wifiQrPayload(apSsid, apPassStr));
    String qrUrl = wifiQrQuickChartUrl(apSsid, apPassStr, 500);
    Serial.println(String("WiFi QR URL (QuickChart, size=500): ") + qrUrl);
  } else {
    Serial.println("WiFi QR payload (copy-paste into QR generator):");
    Serial.println(wifiQrPayload(apSsid, apPassStr));
  }
  Serial.println();

#if ENABLE_BLE_PROV && HAS_WIFI_PROV
  Serial.println("BLE provisioning enabled (WiFiProv). Starting BLE provisioning…");
  // Scaffold only: API varies by core. Keep AP portal as primary path.
#endif

  // Start web server AFTER WiFi mode is set up (prevents queue/semaphore asserts)
  server.begin();

  // Kick off initial scan asynchronously (optional)
  ensureScanStarted();
}

void startConnectedServices() {
  // Auto shutdown of setup AP after provisioning
  stopPortalServices();

  // Apply configured pins
  PIN_OUT = loadInt("out", 6);
  LED_ACTIVE_HIGH = loadBool("ledah", true);
  pinMode(PIN_OUT, OUTPUT);
  breathPeriodMs = (uint32_t)loadInt("br_period", 3000);
  breathMinPct = (uint8_t)loadInt("br_min", 5);
  breathMaxPct = (uint8_t)loadInt("br_max", 100);
  if (breathPeriodMs < 500) breathPeriodMs = 500;
  if (breathPeriodMs > 10000) breathPeriodMs = 10000;
  if (breathMinPct < 1) breathMinPct = 1;
  if (breathMinPct > 99) breathMinPct = 99;
  if (breathMaxPct < 1) breathMaxPct = 1;
  if (breathMaxPct > 100) breathMaxPct = 100;
  if (breathMaxPct <= breathMinPct) breathMaxPct = breathMinPct + 1;
  setOutputMode(loadInt("mode", MODE_OFF));

  // Ensure API token exists once the device is configured and connected.
  String apiToken = loadApiToken();
  if (apiToken.length() == 0) {
    apiToken = makeApiToken();
    prefs.putString("api_token", apiToken);
  }

  // mDNS
  String host = loadString("host", defaultHostName().c_str());
  if (MDNS.begin(host.c_str())) {
    MDNS.addService("http", "tcp", 80);
    Serial.print("mDNS: http://"); Serial.print(host); Serial.println(".local/");
  } else {
    Serial.println("mDNS failed to start");
  }

  Serial.println();
  Serial.println("== Connected ==");
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  // Start server now that STA is connected
  server.begin();
}

// ---------------- HTTP handlers ----------------

void setupHttpHandlers() {
  // Root page: setup or connected view
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!portalMode && !ensureBasicAuth(request)) return;
    if (portalMode) request->send(200, "text/html", setupPage());
    else request->send(200, "text/html", connectedPage());
  });

  // WiFi scan (NON-BLOCKING)
  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!portalMode && !ensureBasicAuth(request)) return;
    if (!portalMode) {
      request->send(409, "application/json", "{\"ok\":false,\"error\":\"Not in setup mode\"}");
      return;
    }

    int st = WiFi.scanComplete();
    if ((st == WIFI_SCAN_RUNNING || st == WIFI_SCAN_FAILED) &&
        lastScanStartedMs && (millis() - lastScanStartedMs > SCAN_MAX_WAIT_MS)) {
      WiFi.scanDelete();
      st = WiFi.scanNetworks(false /*async*/, true /*show_hidden*/);
      lastScanStartedMs = millis();
    }
    DynamicJsonDocument doc(4096);

    if (st == WIFI_SCAN_RUNNING) {
      doc["scanning"] = true;
    } else if (st >= 0) {
      // Scan complete: build list
      JsonArray arr = doc["ssids"].to<JsonArray>();
      for (int i = 0; i < st; i++) {
        String s = WiFi.SSID(i);
        if (s.length()) arr.add(s);
      }
      WiFi.scanDelete();
      doc["scanning"] = false;
    } else {
      // No scan running: start one and report scanning
      ensureScanStarted();
      doc["scanning"] = true;
    }

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  // Save config (only in portal mode), then reboot to apply STA WiFi
  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (!portalMode && !ensureBasicAuth(request)) return;
      String *body = reinterpret_cast<String*>(request->_tempObject);
      if (index == 0) {
        delete body;
        body = new String();
        body->reserve(total);
        request->_tempObject = body;
      }
      if (!body) return;
      body->concat((char*)data, len);

      if (index + len == total) {
        DynamicJsonDocument doc(1024);
        DynamicJsonDocument resp(256);
        String payload = *body;
        delete body;
        request->_tempObject = nullptr;

        if (deserializeJson(doc, payload)) {
          resp["ok"] = false;
          resp["error"] = "Bad JSON";
        } else {
          String ssid = doc["ssid"] | "";
          String pass = doc["pass"] | "";
          String host = doc["host"] | "esp32c6";
          int outPin = doc["out"] | 6;
          bool ledah = doc["ledah"] | true;
          bool usebl = doc["usebl"] | false;
          String authUser = doc["auth_user"] | "admin";
          String authPass = doc["auth_pass"] | "esp32c6";
          String apPass = doc["ap_pass"] | "";
          if (usebl || outPin < 0) outPin = PIN_LED;

          if (!portalMode) {
            resp["ok"] = false;
            resp["error"] = "Not in setup mode";
          } else if (!ssid.length()) {
            resp["ok"] = false;
            resp["error"] = "SSID required";
          } else if (ssid.length() > 32 || pass.length() > 63 || host.length() > 32) {
            resp["ok"] = false;
            resp["error"] = "WiFi or hostname too long";
          } else if (authUser.length() == 0 || authUser.length() > 32) {
            resp["ok"] = false;
            resp["error"] = "Auth user must be 1-32 chars";
          } else if (authPass.length() == 0 || authPass.length() > 64) {
            resp["ok"] = false;
            resp["error"] = "Auth password must be 1-64 chars";
          } else if (apPass.length() > 0 && apPass.length() < 8) {
            resp["ok"] = false;
            resp["error"] = "AP password must be at least 8 chars";
          } else {
            saveConfig(ssid, pass, host, outPin, ledah, usebl, authUser, authPass, apPass);
            resp["ok"] = true;

            String outS;
            serializeJson(resp, outS);
            request->send(200, "application/json", outS);

            delay(250);
            ESP.restart();
            return;
          }
        }

        String outS;
        serializeJson(resp, outS);
        request->send(200, "application/json", outS);
      }
    }
  );

  // Captive portal redirects for OS probes and unknown paths
  server.onNotFound([](AsyncWebServerRequest *request) {
    if (portalMode) {
      if (isCaptiveProbe(request)) {
        captiveRedirect(request);
        return;
      }
      // For everything else in portal mode, redirect to the setup UI
      captiveRedirect(request);
      return;
    }
    request->send(404, "application/json", "{\"ok\":false,\"error\":\"Not found\"}");
  });

  // -------- HTTP API --------

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!ensureApiAuth(request)) return;
    DynamicJsonDocument doc(512);
    doc["ok"] = true;
    doc["mode"] = portalMode ? "ap" : "sta";
    doc["ip"] = portalMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    doc["ssid"] = portalMode ? apSsid : WiFi.SSID();
    doc["hostname"] = loadString("host", defaultHostName().c_str());
    doc["fw_version"] = FW_VERSION;
    doc["out_pin"] = loadInt("out", 6);
    doc["led_active_high"] = loadBool("ledah", true);
    doc["output_mode"] = outputMode == MODE_BREATHING ? "breathing" : (outputMode == MODE_ON ? "on" : "off");
    doc["br_period_ms"] = breathPeriodMs;
    doc["br_min_pct"] = breathMinPct;
    doc["br_max_pct"] = breathMaxPct;
    doc["state"] = (outputMode == MODE_ON);
    if (!portalMode) doc["rssi"] = WiFi.RSSI();

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/api/set", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!ensureApiAuth(request)) return;
    DynamicJsonDocument doc(256);
    if (!request->hasParam("state")) {
      doc["ok"] = false;
      doc["error"] = "Missing state=0|1";
    } else {
      int state = request->getParam("state")->value().toInt();
      setOutputMode(state != 0 ? MODE_ON : MODE_OFF);
      prefs.putInt("mode", outputMode);
      doc["ok"] = true;
      doc["state"] = (state != 0);
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/api/mode", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!ensureApiAuth(request)) return;
    DynamicJsonDocument doc(256);
    if (!request->hasParam("mode")) {
      doc["ok"] = false;
      doc["error"] = "Missing mode=off|on|breathing";
    } else {
      String mode = request->getParam("mode")->value();
      mode.toLowerCase();
      int nextMode = MODE_OFF;
      if (mode == "on" || mode == "1") nextMode = MODE_ON;
      else if (mode == "breathing" || mode == "2") nextMode = MODE_BREATHING;

      if (request->hasParam("period_ms")) {
        uint32_t p = (uint32_t)request->getParam("period_ms")->value().toInt();
        if (p < 500 || p > 10000) {
          doc["ok"] = false;
          doc["error"] = "period_ms must be 500-10000";
          String out;
          serializeJson(doc, out);
          request->send(400, "application/json", out);
          return;
        }
        breathPeriodMs = p;
      }
      if (request->hasParam("min_pct")) {
        int d = request->getParam("min_pct")->value().toInt();
        if (d < 1 || d > 99) {
          doc["ok"] = false;
          doc["error"] = "min_pct must be 1-99";
          String out;
          serializeJson(doc, out);
          request->send(400, "application/json", out);
          return;
        }
        breathMinPct = (uint8_t)d;
      }
      if (request->hasParam("max_pct")) {
        int d = request->getParam("max_pct")->value().toInt();
        if (d < 1 || d > 100) {
          doc["ok"] = false;
          doc["error"] = "max_pct must be 1-100";
          String out;
          serializeJson(doc, out);
          request->send(400, "application/json", out);
          return;
        }
        breathMaxPct = (uint8_t)d;
      }
      if (breathMaxPct <= breathMinPct) {
        doc["ok"] = false;
        doc["error"] = "max_pct must be greater than min_pct";
        String out;
        serializeJson(doc, out);
        request->send(400, "application/json", out);
        return;
      }

      setOutputMode(nextMode);
      prefs.putInt("mode", outputMode);
      prefs.putInt("br_period", (int)breathPeriodMs);
      prefs.putInt("br_min", (int)breathMinPct);
      prefs.putInt("br_max", (int)breathMaxPct);

      doc["ok"] = true;
      doc["output_mode"] = outputMode == MODE_BREATHING ? "breathing" : (outputMode == MODE_ON ? "on" : "off");
      doc["br_period_ms"] = breathPeriodMs;
      doc["br_min_pct"] = breathMinPct;
      doc["br_max_pct"] = breathMaxPct;
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!ensureApiAuth(request)) return;
    DynamicJsonDocument doc(512);
    doc["ok"] = true;
    doc["ssid"] = loadString("ssid", "");
    doc["hostname"] = loadString("host", defaultHostName().c_str());
    doc["out"] = loadInt("out", 6);
    doc["ledah"] = loadBool("ledah", true);
    doc["output_mode"] = outputMode == MODE_BREATHING ? "breathing" : (outputMode == MODE_ON ? "on" : "off");
    doc["br_period_ms"] = breathPeriodMs;
    doc["br_min_pct"] = breathMinPct;
    doc["br_max_pct"] = breathMaxPct;

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!ensureApiAuth(request)) return;
    request->send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    delay(200);
    ESP.restart();
  });

  // -------- OTA (/update) --------
  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!ensureApiAuth(request)) return;
    if (portalMode) {
      request->send(409, "text/plain", "OTA disabled in setup mode. Connect device to WiFi first.");
      return;
    }
    String body;
    body += "<p>Upload a compiled <b>.bin</b> to update firmware.</p>";
    body += "<form method='POST' action='/update' enctype='multipart/form-data'>"
            "<input type='file' name='firmware' accept='.bin' required/>"
            "<div class='btns'><button id='btnUpload' type='submit'>Upload & Update</button></div>"
            "</form>"
            "<p class='hint'>After upload completes, the device will reboot.</p>";
    String script;
    script += "document.addEventListener('DOMContentLoaded',()=>{"
              "  const b=document.getElementById('btnUpload');"
              "  const f=document.querySelector('form');"
              "  if(b&&f){f.addEventListener('submit',()=>{b.classList.add('active'); b.classList.add('busy');});}"
              "});";
    AsyncWebServerResponse *resp = request->beginResponse(
      200, "text/html", pageShell("OTA Update", body, script, true)
    );
    resp->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    resp->addHeader("Pragma", "no-cache");
    resp->addHeader("Expires", "0");
    request->send(resp);
  });

  server.on(
    "/update",
    HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (!ensureApiAuth(request)) return;
      if (portalMode) {
        request->send(409, "application/json", "{\"ok\":false,\"error\":\"OTA disabled in setup mode\"}");
        return;
      }
      bool ok = !Update.hasError();
      bool wantsJson = false;
      if (request->hasHeader("Accept")) {
        String accept = request->getHeader("Accept")->value();
        accept.toLowerCase();
        wantsJson = accept.indexOf("application/json") >= 0;
      }
      if (wantsJson) {
        AsyncWebServerResponse *resp = request->beginResponse(
          200, "application/json",
          ok ? "{\"ok\":true,\"rebooting\":true}" : "{\"ok\":false,\"error\":\"Update failed\"}"
        );
        resp->addHeader("Connection", "close");
        resp->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        request->send(resp);
      } else {
        String body;
        if (ok) {
          body += "<p>Update complete. Rebooting…</p>";
          body += "<p class='hint'>You will be redirected to the main page.</p>";
          body += "<div class='btns'><button onclick=\"location.href='/'\">Go to Home</button></div>";
          body += "<script>setTimeout(()=>{location.href='/'},8000);</script>";
        } else {
          body += "<p class='bad'>Update failed.</p>";
          body += "<div class='btns'><button onclick=\"location.href='/update'\">Try Again</button></div>";
        }
        AsyncWebServerResponse *resp = request->beginResponse(
          200, "text/html", pageShell("OTA Update", body, "", true)
        );
        resp->addHeader("Connection", "close");
        resp->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        request->send(resp);
      }
      delay(1200);
      ESP.restart();
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (!ensureApiAuth(request)) return;
      if (index == 0) {
        Serial.printf("OTA start: %s\n", filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
        }
      }
      if (len) {
        if (Update.write(data, len) != len) {
          Update.printError(Serial);
        }
      }
      if (final) {
        if (!Update.end(true)) {
          Update.printError(Serial);
        } else {
          Serial.printf("OTA done: %u bytes\n", (unsigned)(index + len));
        }
      }
    }
  );
}

// ---------------- Arduino setup/loop ----------------

void setup() {
  // ROM bootloader messages print before this point (kept intentionally)
  Serial.begin(115200);
  // Give the USB CDC/UART bridge a moment to come up so early logs aren't missed.
  unsigned long _serialWaitStart = millis();
  while (!Serial && (millis() - _serialWaitStart) < 1500) { delay(10); }
  delay(200);

  // Identification
  Serial.println();
  Serial.println("ESP32-C6 Output Controller");
  Serial.println("=========================");
  Serial.print("Board: "); Serial.println(BOARD_NAME);
  Serial.print("Device S/N: "); Serial.println(makeMacHex12());
  Serial.println();

  pinMode(PIN_LED, OUTPUT);
  ledWrite(false);

  pinMode(PIN_BOOT, INPUT_PULLUP);

  prefs.begin("cfg", false);

  // Load LED polarity early so reset feedback is correct
  LED_ACTIVE_HIGH = loadBool("ledah", true);

  // Build SSID and password for Setup AP
  // Use the device MAC to avoid collisions between multiple boards.
  apSsid = "C6-SETUP-" + makeMacHex12();
  if (!prefs.isKey("ap_pass")) {
    apPassStr = makeFriendlyApPassword();
    prefs.putString("ap_pass", apPassStr);
  } else {
    apPassStr = loadString("ap_pass", "");
  }

  // Setup output pin default early
  {
    bool usebl = loadBool("usebl", false);
    PIN_OUT = usebl ? PIN_LED : loadInt("out", 6);
  }
  pinMode(PIN_OUT, OUTPUT);
  outputWrite(false);

  // Register HTTP routes ONCE (safe before WiFi), but do not begin server yet.
  setupHttpHandlers();

  // Boot-time factory reset check
  checkBootTimeFactoryReset();

  // Try WiFi connect; else start portal
  Serial.println("Connecting to saved WiFi (if any)...");
  if (connectWifiWithSaved()) {
    startConnectedServices();
  } else {
    Serial.println("WiFi connect failed or not configured. Starting setup AP + captive portal...");
    startCaptivePortal();
  }
}

void loop() {
  // Captive portal DNS processing
  if (portalMode) {
    dnsServer.processNextRequest();

    // Auto-timeout for setup AP
    if (SETUP_AP_TIMEOUT_MS > 0 && (millis() - portalStartedMs) > SETUP_AP_TIMEOUT_MS) {
      Serial.println("Setup AP timeout reached. Rebooting to retry STA...");
      delay(200);
      ESP.restart();
    }
  }

  breathingTick();

  // Runtime factory reset long-press
  handleResetLongPress();

  delay(5);
}
