// SmallTV Pro probe firmware.
// Read-only toward flash and LittleFS (never formats); its only write paths
// are OTA (/update) and switching the boot slot back to the stock app (/boot_other).
//
//   GET  /                 info: chip, partitions, running slot, LittleFS files
//   GET  /partitions.json  partition table
//   GET  /flash            raw flash dump (?offset=0&size=<bytes>, default whole chip)
//   GET  /fs?path=/x       read a LittleFS file
//   POST /update           OTA, multipart field "firmware" (same as stock)
//   GET  /boot_other       boot the other OTA slot (stock firmware) and reboot
//   GET  /reboot

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Update.h>
#include <esp_flash.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <TFT_eSPI.h>
#include "secrets.h"

static const int PIN_BACKLIGHT = 25;
static const int BL_CHANNEL = 0;
static const int PIN_TOUCH = 32;

TFT_eSPI tft;

static const char *AP_SSID = "SmallTV-Probe";
static const char *AP_PASS = "smalltv-probe";
static const char *HOSTNAME = "smalltv-probe";

WebServer server(80);
static bool fsMounted = false;
static String fsLabel;

static const char *partType(const esp_partition_t *p) {
  if (p->type == ESP_PARTITION_TYPE_APP) {
    if (p->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) return "app/factory";
    if (p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) return "app/ota";
    return "app/?";
  }
  switch (p->subtype) {
    case ESP_PARTITION_SUBTYPE_DATA_OTA: return "data/ota";
    case ESP_PARTITION_SUBTYPE_DATA_NVS: return "data/nvs";
    case ESP_PARTITION_SUBTYPE_DATA_COREDUMP: return "data/coredump";
    case ESP_PARTITION_SUBTYPE_DATA_SPIFFS: return "data/spiffs";
    case ESP_PARTITION_SUBTYPE_DATA_FAT: return "data/fat";
    case ESP_PARTITION_SUBTYPE_DATA_PHY: return "data/phy";
    default: return "data/?";
  }
}

// Mount the first spiffs-subtype partition that holds a valid LittleFS. Never formats.
static void mountFs() {
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
  for (; it; it = esp_partition_next(it)) {
    const esp_partition_t *p = esp_partition_get(it);
    if (LittleFS.begin(false, "/littlefs", 10, p->label)) {
      fsMounted = true;
      fsLabel = p->label;
      break;
    }
  }
  esp_partition_iterator_release(it);
}

// Stock firmware keeps Wi-Fi in /.sys/config.json as {"a":"<ssid>","p":"<pass>"}.
static bool readStockWifi(String &ssid, String &pass) {
  if (!fsMounted) return false;
  File f = LittleFS.open("/.sys/config.json", "r");
  if (!f) return false;
  String s = f.readString();
  f.close();
  auto field = [&](const char *key) -> String {
    String k = String("\"") + key + "\"";
    int i = s.indexOf(k);
    if (i < 0) return "";
    i = s.indexOf('"', s.indexOf(':', i + k.length()) + 1);
    int j = s.indexOf('"', i + 1);
    return (i < 0 || j < 0) ? "" : s.substring(i + 1, j);
  };
  ssid = field("a");
  pass = field("p");
  return ssid.length() > 0;
}

static bool tryConnect(const String &ssid, const String &pass, uint32_t timeoutMs) {
  Serial.printf("WiFi: connecting to %s\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(250);
  }
  WiFi.disconnect();
  return false;
}

static void startWifi() {
  WiFi.persistent(false);  // do not touch Wi-Fi config stored in NVS by stock firmware
  WiFi.mode(WIFI_AP_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.softAP(AP_SSID, AP_PASS);  // always on, 192.168.4.1
  WiFi.setAutoReconnect(true);

  String ssid, pass;
  bool ok = false;
  if (readStockWifi(ssid, pass)) ok = tryConnect(ssid, pass, 20000);
  if (!ok) ok = tryConnect(FALLBACK_SSID, FALLBACK_PASS, 20000);
  if (ok) Serial.printf("WiFi: %s\n", WiFi.localIP().toString().c_str());
  else {
    Serial.println("WiFi: STA failed, AP only");
    WiFi.begin(FALLBACK_SSID, FALLBACK_PASS);  // keep retrying in background
  }
}

static void listDir(String &out, const char *dir) {
  File d = LittleFS.open(dir);
  if (!d || !d.isDirectory()) return;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    String path = String(dir) + (strcmp(dir, "/") ? "/" : "") + f.name();
    if (f.isDirectory()) listDir(out, path.c_str());
    else out += "  " + path + "  " + f.size() + "\n";
  }
}

static String partitionsJson() {
  String j = "[";
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
  for (bool first = true; it; it = esp_partition_next(it), first = false) {
    const esp_partition_t *p = esp_partition_get(it);
    char buf[160];
    snprintf(buf, sizeof buf, "%s{\"label\":\"%s\",\"type\":%d,\"subtype\":%d,\"kind\":\"%s\",\"offset\":%u,\"size\":%u}",
             first ? "" : ",", p->label, p->type, p->subtype, partType(p), p->address, p->size);
    j += buf;
  }
  esp_partition_iterator_release(it);
  return j + "]";
}

static void handleRoot() {
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *boot = esp_ota_get_boot_partition();
  String o = "SmallTV Pro probe\n\n";
  o += "Chip: " + String(ESP.getChipModel()) + " rev " + ESP.getChipRevision() + ", cores " + ESP.getChipCores() + "\n";
  o += "MAC: " + WiFi.macAddress() + "\n";
  o += "Flash: " + String(ESP.getFlashChipSize()) + " bytes, " + ESP.getFlashChipSpeed() / 1000000 + " MHz\n";
  o += "PSRAM: " + String(ESP.getPsramSize()) + "\n";
  o += "Heap free: " + String(ESP.getFreeHeap()) + "\n";
  o += "SDK: " + String(ESP.getSdkVersion()) + "\n";
  o += "Running: " + String(run->label) + ", boot: " + String(boot->label) + "\n";
  o += "STA IP: " + WiFi.localIP().toString() + ", RSSI " + WiFi.RSSI() + "\n\n";

  o += "Partitions:\n";
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
  for (; it; it = esp_partition_next(it)) {
    const esp_partition_t *p = esp_partition_get(it);
    char buf[128];
    snprintf(buf, sizeof buf, "  %-10s %-14s 0x%06x  0x%06x", p->label, partType(p), p->address, p->size);
    o += buf;
    esp_app_desc_t d;
    if (p->type == ESP_PARTITION_TYPE_APP && esp_ota_get_partition_description(p, &d) == ESP_OK)
      o += String("  [") + d.project_name + " " + d.version + " " + d.date + "]";
    o += "\n";
  }
  esp_partition_iterator_release(it);

  o += "\nLittleFS (" + (fsMounted ? fsLabel + ", " + LittleFS.usedBytes() + "/" + LittleFS.totalBytes() : String("not mounted")) + "):\n";
  if (fsMounted) listDir(o, "/");
  server.send(200, "text/plain", o);
}

static void handleFlash() {
  uint32_t chip = ESP.getFlashChipSize();
  uint32_t off = server.hasArg("offset") ? strtoul(server.arg("offset").c_str(), NULL, 0) : 0;
  uint32_t size = server.hasArg("size") ? strtoul(server.arg("size").c_str(), NULL, 0) : chip - off;
  if (off >= chip || size == 0 || off + size > chip) {
    server.send(400, "text/plain", "bad range");
    return;
  }
  static uint8_t buf[4096];
  WiFiClient c = server.client();
  server.setContentLength(size);
  server.sendHeader("Content-Disposition", "attachment; filename=flash.bin");
  server.send(200, "application/octet-stream", "");
  for (uint32_t a = off; a < off + size && c.connected();) {
    uint32_t n = min<uint32_t>(sizeof buf, off + size - a);
    if (esp_flash_read(NULL, buf, a, n) != ESP_OK) break;
    if (c.write(buf, n) != n) break;
    a += n;
  }
}

static void handleFs() {
  String path = server.arg("path");
  if (!fsMounted || !path.startsWith("/") || !LittleFS.exists(path)) {
    server.send(404, "text/plain", "not found");
    return;
  }
  File f = LittleFS.open(path, "r");
  server.streamFile(f, "application/octet-stream");
  f.close();
}

static void handleUpdateDone() {
  bool ok = !Update.hasError();
  server.sendHeader("Connection", "close");
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK, rebooting" : String("FAIL: ") + Update.errorString());
  if (ok) {
    delay(500);
    ESP.restart();
  }
}

static void handleUpdateUpload() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    Serial.printf("OTA: %s\n", u.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) Update.printError(Serial);
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (Update.write(u.buf, u.currentSize) != u.currentSize) Update.printError(Serial);
  } else if (u.status == UPLOAD_FILE_END) {
    if (Update.end(true)) Serial.printf("OTA: done, %u bytes\n", u.totalSize);
    else Update.printError(Serial);
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
  }
}

static const char UPDATE_PAGE[] PROGMEM =
    "<!DOCTYPE html><html><body><h3>SmallTV probe OTA</h3>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='firmware' accept='.bin'> <input type='submit' value='Update'></form></body></html>";

static void handleBootOther() {
  const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
  esp_app_desc_t d;
  if (!next || esp_ota_get_partition_description(next, &d) != ESP_OK) {
    server.send(500, "text/plain", "other slot has no valid app");
    return;
  }
  esp_err_t e = esp_ota_set_boot_partition(next);
  if (e != ESP_OK) {
    server.send(500, "text/plain", String("set_boot failed: ") + esp_err_to_name(e));
    return;
  }
  server.send(200, "text/plain", String("Booting ") + next->label + " (" + d.project_name + " " + d.version + ")");
  delay(500);
  ESP.restart();
}

// Same sequence as stock: ledcSetup(0, 5000, 8); ledcAttachPin(25, 0).
static void displayInit() {
  ledcSetup(BL_CHANNEL, 5000, 8);
  ledcAttachPin(PIN_BACKLIGHT, BL_CHANNEL);
  ledcWrite(BL_CHANNEL, 200);
  tft.init();
  const uint16_t colors[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE};
  for (uint16_t c : colors) {
    tft.fillScreen(c);
    delay(400);
  }
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString("SmallTV probe", 10, 10, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("R G B", 10, 50, 4);
  tft.fillRect(10, 80, 70, 30, TFT_RED);
  tft.fillRect(85, 80, 70, 30, TFT_GREEN);
  tft.fillRect(160, 80, 70, 30, TFT_BLUE);
  tft.drawRect(0, 0, 240, 240, TFT_YELLOW);  // shows any row/column offset
}

static void displayStatus() {
  static uint32_t last = 0;
  if (millis() - last < 500) return;
  last = millis();
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextPadding(220);
  tft.drawString("IP " + WiFi.localIP().toString(), 10, 125, 4);
  tft.drawString("AP " + WiFi.softAPIP().toString(), 10, 155, 2);
  tft.drawString("touch " + String(touchRead(PIN_TOUCH)), 10, 180, 4);
  tft.drawString("up " + String(millis() / 1000) + "s", 10, 210, 2);
  tft.setTextPadding(0);
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nSmallTV probe");
  esp_ota_mark_app_valid_cancel_rollback();
  displayInit();
  mountFs();
  startWifi();
  MDNS.begin(HOSTNAME);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/partitions.json", HTTP_GET, [] { server.send(200, "application/json", partitionsJson()); });
  server.on("/flash", HTTP_GET, handleFlash);
  server.on("/fs", HTTP_GET, handleFs);
  server.on("/update", HTTP_GET, [] { server.send_P(200, "text/html", UPDATE_PAGE); });
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.on("/boot_other", HTTP_GET, handleBootOther);
  server.on("/reboot", HTTP_GET, [] {
    server.send(200, "text/plain", "rebooting");
    delay(500);
    ESP.restart();
  });
  server.begin();
}

void loop() {
  server.handleClient();
  displayStatus();
  delay(1);
}
