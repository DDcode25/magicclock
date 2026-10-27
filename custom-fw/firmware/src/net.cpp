#include "net.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include "board.h"
#include "store.h"

namespace net {

static const uint32_t CONNECT_TIMEOUT_MS = 20000;
static const uint32_t LINK_LOST_AP_MS = 120000;  // open the AP after this long without STA

static DNSServer dns;
static bool ap = false;
static String staSsid;
static uint32_t lastConnectedMs = 0;

static void startAp() {
  if (ap) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID);
  dns.start(53, "*", WiFi.softAPIP());
  ap = true;
  Serial.printf("WiFi: AP %s at %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

static void stopAp() {
  if (!ap) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  ap = false;
}

void begin() {
  WiFi.persistent(false);  // credentials live in config.json, not NVS
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);

  staSsid = store::getStr("/.sys/config.json", "a", "");
  String pass = store::getStr("/.sys/config.json", "p", "");
  bool ok = false;
  if (staSsid.length()) {
    Serial.printf("WiFi: connecting to %s\n", staSsid.c_str());
    WiFi.begin(staSsid.c_str(), pass.c_str());
    uint32_t t0 = millis();
    while (millis() - t0 < CONNECT_TIMEOUT_MS && WiFi.status() != WL_CONNECTED) delay(200);
    ok = WiFi.status() == WL_CONNECTED;
  }
  if (ok) {
    lastConnectedMs = millis();
    Serial.printf("WiFi: %s\n", WiFi.localIP().toString().c_str());
  } else {
    startAp();
  }
  MDNS.begin(HOSTNAME);
  MDNS.addService("http", "tcp", 80);
  applyTime();
}

void loop() {
  if (ap) dns.processNextRequest();
  if (WiFi.status() == WL_CONNECTED) {
    lastConnectedMs = millis();
  } else if (!ap && millis() - lastConnectedMs > LINK_LOST_AP_MS) {
    startAp();
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
String ip() { return connected() ? WiFi.localIP().toString() : (ap ? WiFi.softAPIP().toString() : String("-")); }
String ssid() { return staSsid; }

void applyTime() {
  // Auto: Europe/Kyiv with DST (EET/EEST). Manual: fixed offset; POSIX TZ sign is inverted.
  char tz[40] = "EET-2EEST,M3.5.0/3,M10.5.0/4";
  int off = settings.tzOffsetMin;
  if (settings.tzManual)
    snprintf(tz, sizeof tz, "UTC%c%d:%02d", off >= 0 ? '-' : '+', abs(off) / 60, abs(off) % 60);
  configTzTime(tz, settings.ntpServer.c_str(), "pool.ntp.org", "time.windows.com");
}

bool timeValid() { return time(nullptr) > 1700000000; }

static SemaphoreHandle_t fetchMutex = xSemaphoreCreateMutex();
void fetchLock() { xSemaphoreTake(fetchMutex, portMAX_DELAY); }
void fetchUnlock() { xSemaphoreGive(fetchMutex); }

String scanJson() {
  int n = WiFi.scanNetworks(false, false);
  String j = "{\"aps\":[";
  for (int i = 0; i < n; i++) {
    int q = constrain(2 * (WiFi.RSSI(i) + 100), 0, 100);
    String s = WiFi.SSID(i);
    s.replace("\\", "\\\\");
    s.replace("\"", "\\\"");
    if (i) j += ",";
    j += "{\"ss\":\"" + s + "\",\"r\":" + q + ",\"e\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1) + "}";
  }
  WiFi.scanDelete();
  return j + "]}";
}

bool saveWifi(const String &s, const String &p) {
  JsonDocument doc;
  doc["a"] = s;
  doc["p"] = p;
  return store::save("/.sys/config.json", doc);
}

}  // namespace net
