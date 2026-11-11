#include "net.h"
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <coredecls.h>
#include <time.h>
#include "board.h"
#include "diag.h"
#include "fonts.h"
#include "store.h"

namespace net {

static const uint32_t CONNECT_TIMEOUT_MS = 20000;
static const uint32_t LINK_LOST_AP_MS = 120000;  // open the AP after this long without STA
static const uint32_t RETRY_MS = 60000;          // offline: look for a known network this often
static const int KNOWN_MAX = 5;
static const char KNOWN_FILE[] PROGMEM = "/.sys/wifis.json";  // {"list":[{"a":ssid,"p":pass},...]}, newest first

static DNSServer dns;
static bool ap = false;
static String staSsid;
static uint32_t lastConnectedMs = 0, lastRetryMs = 0;

struct Known {
  String ssid, pass;
};

static int loadKnown(Known *out) {
  JsonDocument doc = store::load(KNOWN_FILE);
  int n = 0;
  for (JsonObject o : doc["list"].as<JsonArray>()) {
    if (n == KNOWN_MAX) break;
    String a = o["a"] | "";
    if (a.length()) out[n].ssid = a, out[n].pass = o["p"] | "", n++;
  }
  return n;
}

// Puts ssid first (most recent), drops its old entry and anything past KNOWN_MAX.
static bool remember(const String &ssid, const String &pass) {
  Known k[KNOWN_MAX];
  int n = loadKnown(k);
  JsonDocument doc;
  JsonArray list = doc["list"].to<JsonArray>();
  JsonObject first = list.add<JsonObject>();
  first["a"] = ssid, first["p"] = pass;
  for (int i = 0; i < n && list.size() < KNOWN_MAX; i++) {
    if (k[i].ssid == ssid) continue;
    JsonObject o = list.add<JsonObject>();
    o["a"] = k[i].ssid, o["p"] = k[i].pass;
  }
  return store::save(KNOWN_FILE, doc);
}

static void startAp() {
  if (ap) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID);
  dns.start(53, "*", WiFi.softAPIP());
  ap = true;
  Serial.printf_P(PSTR("WiFi: AP %s at %s\n"), AP_SSID, WiFi.softAPIP().toString().c_str());
}

static void stopAp() {
  if (!ap) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  ap = false;
}

// Blocking scan. A STA that keeps retrying a missing network makes scans fail, so the retries are
// stopped first (the next reconnect starts them again).
static int scan() {
  if (WiFi.status() != WL_CONNECTED) WiFi.disconnect(false);
  int n = WiFi.scanNetworks(false, false);
  if (n < 0) {
    delay(300);
    n = WiFi.scanNetworks(false, false);
  }
  return n;
}

// Scans and starts connecting to the strongest known network in range.
static bool connectKnown() {
  Known k[KNOWN_MAX];
  int n = loadKnown(k);
  if (!n) return false;
  int found = scan(), best = -1, bestRssi = -1000;
  for (int i = 0; i < found; i++)
    for (int j = 0; j < n; j++)
      if (WiFi.SSID(i) == k[j].ssid && WiFi.RSSI(i) > bestRssi) best = j, bestRssi = WiFi.RSSI(i);
  WiFi.scanDelete();
  if (best < 0) {
    Serial.println(F("WiFi: no known network in range"));
    return false;
  }
  staSsid = k[best].ssid;
  Serial.printf_P(PSTR("WiFi: connecting to %s (%d dBm)\n"), staSsid.c_str(), bestRssi);
  WiFi.begin(k[best].ssid.c_str(), k[best].pass.c_str());
  return true;
}

// The stock firmware kept the network in the SDK's own flash config; take it over on the first boot so
// the clock stays on the network after the switch.
static void importSdkWifi() {
  struct station_config conf;
  if (!wifi_station_get_config_default(&conf) || !conf.ssid[0]) return;
  char ssid[33] = {0}, pass[65] = {0};
  memcpy(ssid, conf.ssid, 32);
  memcpy(pass, conf.password, 64);
  Serial.printf_P(PSTR("WiFi: imported %s from the SDK config\n"), ssid);
  saveWifi(ssid, pass);
}

void begin() {
  if (!store::mounted() || store::getStr(PSTR("/.sys/config.json"), PSTR("a"), PSTR("")).isEmpty()) importSdkWifi();
  WiFi.persistent(false);  // credentials live in /.sys, not the SDK config
  WiFi.mode(WIFI_STA);
  // No modem sleep: asleep between beacons the radio answered 5..225 ms late and an OTA upload
  // crawled at a few hundred bytes/s. Mains powered, so the extra ~15 mA do not matter.
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.hostname(HOSTNAME);
  WiFi.setAutoReconnect(true);

  // The stock config.json holds the network saved last; older firmware knew only that one.
  String a = store::getStr(PSTR("/.sys/config.json"), PSTR("a"), PSTR(""));
  Known k[KNOWN_MAX];
  int n = loadKnown(k);
  if (a.length() && (!n || k[0].ssid != a)) remember(a, store::getStr(PSTR("/.sys/config.json"), PSTR("p"), PSTR("")));
  staSsid = a;

  bool ok = false;
  if (connectKnown()) {
    uint32_t t0 = millis();
    while (millis() - t0 < CONNECT_TIMEOUT_MS && WiFi.status() != WL_CONNECTED) delay(200);
    ok = WiFi.status() == WL_CONNECTED;
  }
  lastRetryMs = millis();
  if (ok) {
    lastConnectedMs = millis();
    Serial.printf_P(PSTR("WiFi: %s\n"), WiFi.localIP().toString().c_str());
  } else {
    startAp();
  }
  applyTime();
}

void loop() {
  if (ap) dns.processNextRequest();
  if (WiFi.status() == WL_CONNECTED) {
    lastConnectedMs = millis();
  } else {
    if (!ap && millis() - lastConnectedMs > LINK_LOST_AP_MS) startAp();
    // Moved to another place, or the router is back: look for any known network. Not while someone
    // is on the setup AP (a scan and a new STA channel would drop them).
    if (millis() - lastConnectedMs > 30000 && millis() - lastRetryMs > RETRY_MS &&
        (!ap || WiFi.softAPgetStationNum() == 0)) {
      connectKnown();
      lastRetryMs = millis();
    }
  }
  // Once the STA link is back and the AP has no clients, close it again.
  static uint32_t lastCheck = 0;
  if (ap && WiFi.status() == WL_CONNECTED && millis() - lastCheck > 60000) {
    lastCheck = millis();
    if (WiFi.softAPgetStationNum() == 0) stopAp();
  }
}

bool connected() { return WiFi.status() == WL_CONNECTED; }
bool apActive() { return ap; }
String ip() { return connected() ? WiFi.localIP().toString() : (ap ? WiFi.softAPIP().toString() : String('-')); }
String ssid() { return staSsid; }

void applyTime() {
  // Auto: Europe/Kyiv with DST (EET/EEST). Manual: fixed offset; POSIX TZ sign is inverted.
  char tz[40] = "EET-2EEST,M3.5.0/3,M10.5.0/4";
  int off = settings.tzOffsetMin;
  if (settings.tzManual)
    snprintf(tz, sizeof tz, "UTC%c%d:%02d", off >= 0 ? '-' : '+', abs(off) / 60, abs(off) % 60);
  // String overload: the core keeps copies (the const char * one keeps only the pointers, and
  // settings.ntpServer is reallocated by every settings reload).
  configTime(tz, settings.ntpServer, String(F("pool.ntp.org")), String(F("time.windows.com")));
}

bool timeValid() { return time(nullptr) > 1700000000; }

// Fetches run one at a time from loop() (single thread); these only feed the diagnostics.
static uint32_t heapAtLock = 0;
void fetchLock() {
  releaseFont();  // every KB counts: a TLS session needs ~25 KB next to the parsed JSON
  heapAtLock = ESP.getFreeHeap();
}
void fetchUnlock() {
  diag::fetchDone(heapAtLock);
  diag::crumb(diag::FETCH, PSTR("idle"));
}

String scanJson() {
  int n = scan();
  String j = F("{\"aps\":[");
  for (int i = 0; i < n; i++) {
    int q = constrain(2 * (WiFi.RSSI(i) + 100), 0, 100);
    String s = WiFi.SSID(i);
    s.replace("\\", "\\\\");
    s.replace("\"", "\\\"");
    if (i) j += ',';
    j += F("{\"ss\":\"") + s + F("\",\"r\":") + q + F(",\"e\":") + (WiFi.encryptionType(i) == ENC_TYPE_NONE ? 0 : 1) + '}';
  }
  WiFi.scanDelete();
  lastRetryMs = millis();  // the STA retries stopped for the scan: reconnect after RETRY_MS
  return j + F("]}");
}

bool saveWifi(const String &s, const String &p) {
  JsonDocument doc;
  doc["a"] = s;
  doc["p"] = p;
  return store::save(PSTR("/.sys/config.json"), doc) && remember(s, p);
}

String knownJson() {
  Known k[KNOWN_MAX];
  int n = loadKnown(k);
  JsonDocument doc;
  JsonArray list = doc["list"].to<JsonArray>();
  for (int i = 0; i < n; i++) list.add(k[i].ssid);
  doc[F("current")] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String();
  String out;
  serializeJson(doc, out);
  return out;
}

bool forgetWifi(const String &ssid) {
  Known k[KNOWN_MAX];
  int n = loadKnown(k);
  JsonDocument doc;
  JsonArray list = doc["list"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    if (k[i].ssid == ssid) continue;
    JsonObject o = list.add<JsonObject>();
    o["a"] = k[i].ssid, o["p"] = k[i].pass;
  }
  return store::save(KNOWN_FILE, doc);
}

}  // namespace net
