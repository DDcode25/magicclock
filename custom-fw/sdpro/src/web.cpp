#include "web.h"
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <Updater.h>
#include <map>
#include <vector>
#include "alerts.h"
#include "apps.h"
#include "board.h"
#include "net.h"
#include "outage.h"
#include "store.h"
#include "ui.h"
#include "diag.h"
#include "weather.h"
#include "web_index.h"

namespace web {

static ESP8266WebServer server(80);

static void ok() { server.send(200, "text/plain", "OK"); }

static String urlDecode(const String &s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '%' && i + 2 < s.length()) {
      out += (char)strtol(s.substring(i + 1, i + 3).c_str(), nullptr, 16);
      i += 2;
    } else {
      out += c == '+' ? ' ' : c;
    }
  }
  return out;
}

static const char *contentType(const String &path) {
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".gif")) return "image/gif";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".ico")) return "image/x-icon";
  if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  return "application/octet-stream";
}

// ---- /set ---------------------------------------------------------------------------

enum Kind { STR, INT, INV_STR, INV_INT };  // INV: stored as 255 - percent (brightness)

struct Field {
  const char *param, *file, *key;
  Kind kind;
};

// SD PRO: only the settings of the apps built here (the stock UI's crypto, stock, album and monitor
// fields are ignored).
static const Field FIELDS[] = {
    {"cd1", "city.json", "cd1", STR},
    {"cd2", "city.json", "cd2", STR},
    {"nickname", "city.json", "nickname", STR},
    {"interval", "city.json", "w_i", INT},
    {"w_u", "unit.json", "w_u", STR},
    {"t_u", "unit.json", "t_u", STR},
    {"p_u", "unit.json", "p_u", STR},
    {"hc", "timecolor.json", "hc", STR},
    {"mc", "timecolor.json", "mc", STR},
    {"sc", "timecolor.json", "sc", STR},
    {"hour", "hour12.json", "h", STR},
    {"day", "day.json", "day", STR},
    {"sec_mode", "clock.json", "sm", INT},
    {"tz_offset", "tz.json", "offset", INT},
    {"ntp", "ntp.json", "ntp", STR},
    {"brt", "brt.json", "brt", INV_STR},
    {"t1", "timebrt.json", "t1", INT},
    {"t2", "timebrt.json", "t2", INT},
    {"b1", "timebrt.json", "b1", INT},
    {"b2", "timebrt.json", "b2", INV_INT},
    {"en", "timebrt.json", "en", INT},
    {"theme", "app.json", "theme", STR},
    // UA Smart Box: air alerts app
    {"al_src", "alerts.json", "src", STR},
    {"al_token", "alerts.json", "token", STR},
    {"al_i", "alerts.json", "i", INT},
    {"al_home", "alerts.json", "home", INT},
    {"al_cc", "alerts.json", "cc", STR},
    {"al_auto", "alerts.json", "auto", INT},
    // UA Smart Box: power outage schedule
    {"out_src", "outage.json", "src", STR},
    {"out_grp", "outage.json", "grp", STR},
    {"out_i", "outage.json", "i", INT},
    {"out_hc", "outage.json", "hc", STR},
    {"out_sq", "outage.json", "sq", INT},
};

// "cin3" -> true, idx 3 for prefix "cin"
static bool indexed(const String &name, const char *prefix, int max) {
  size_t n = strlen(prefix);
  if (!name.startsWith(prefix) || name.length() == n) return false;
  for (size_t i = n; i < name.length(); i++)
    if (!isDigit(name[i])) return false;
  return name.substring(n).toInt() < max;
}

class Batch {
 public:
  JsonDocument &doc(const String &file) {
    auto it = docs.find(file);
    if (it == docs.end()) it = docs.emplace(file, store::load(("/.sys/" + file).c_str())).first;
    return it->second;
  }
  void save() {
    for (auto &kv : docs) store::save(("/.sys/" + kv.first).c_str(), kv.second);
  }

 private:
  std::map<String, JsonDocument> docs;
};

// Assign through doc[key] directly: converting it to JsonVariant first would not create
// a missing key, and the value would be silently dropped.
static void setValue(JsonDocument &doc, const char *key, const String &v, Kind kind) {
  switch (kind) {
    case STR: doc[key] = v; break;
    case INT: doc[key] = v.toInt(); break;
    case INV_STR: doc[key] = String(255 - constrain(v.toInt(), 0, 100)); break;
    case INV_INT: doc[key] = 255 - constrain(v.toInt(), 0, 100); break;
  }
}

static bool clearDir(const char *dir) {
  File d = LittleFS.open(dir, "r");
  if (!d || !d.isDirectory()) return false;
  std::vector<String> files;
  for (File f = d.openNextFile(); f; f = d.openNextFile())
    if (!f.isDirectory()) files.push_back(String(dir) + "/" + f.name());
  for (auto &p : files) LittleFS.remove(p);
  return true;
}

static void restartSoon(const char *why) {
  ui::message(why, "перезапуск…");
  delay(800);
  ESP.restart();
}

static void handleSet() {
  diag::crumb(diag::WEB, "web /set");
  // Actions first.
  if (server.hasArg("page")) {
    ui::next(server.arg("page").toInt());
    return ok();
  }
  if (server.hasArg("enter")) {
    ui::enter();
    return ok();
  }
  if (server.hasArg("open_app")) {
    return ui::open(server.arg("open_app")) ? ok() : server.send(400, "text/plain", "app disabled or unknown");
  }
  if (server.hasArg("reboot")) {
    ok();
    restartSoon("Перезапуск");
  }
  if (server.hasArg("reset")) {
    // Factory reset of settings; Wi-Fi (config.json) is kept so the device stays reachable.
    ok();
    File d = LittleFS.open("/.sys", "r");
    std::vector<String> files;
    for (File f = d.openNextFile(); f; f = d.openNextFile())
      if (strcmp(f.name(), "config.json")) files.push_back(String("/.sys/") + f.name());
    for (auto &p : files) LittleFS.remove(p);
    restartSoon("Скидання");
  }
  if (server.hasArg("clear")) {
    String kind = server.arg("clear");
    bool done = kind == "gif" ? clearDir("/image/80x80.gif") : kind == "image" ? clearDir("/image") : false;
    return done ? ok() : server.send(400, "text/plain", "bad clear");
  }

  Batch batch;
  bool timeChanged = false, alertsChanged = false, weatherChanged = false, outageChanged = false;
  for (int i = 0; i < server.args(); i++) {
    String name = server.argName(i), v = server.arg(i);
    if (name == "plain") continue;
    bool handled = false;
    for (const Field &f : FIELDS) {
      if (name == f.param) {
        setValue(batch.doc(f.file), f.key, v, f.kind);
        if (!strcmp(f.file, "outage.json")) outageChanged = true;
        if (!strcmp(f.file, "city.json") || !strcmp(f.file, "unit.json") || !strcmp(f.file, "key.json")) weatherChanged = true;
        handled = true;
        break;
      }
    }
    if (handled) {
      if (name == "cd1") batch.doc("city.json")["ct1"] = v;
      if (name == "tz_offset" || name == "ntp") timeChanged = true;
      if (name.startsWith("al_")) alertsChanged = true;
      continue;
    }
    if (name == "tz_mode") {
      batch.doc("tz.json")["mode"] = v == "manual" || v == "1" ? 1 : 0;
      timeChanged = true;
    } else if (name.startsWith("app_")) {
      batch.doc("app.json")["apps"][name.substring(4)] = v.toInt();
    } else if (name.startsWith("dur_")) {  // SD PRO: seconds on screen per app (0 = stays)
      batch.doc("app.json")["dur"][name.substring(4)] = constrain(v.toInt(), 0, 3600);
    } else {
      Serial.printf("set: unknown %s=%s\n", name.c_str(), v.c_str());
    }
  }
  batch.save();
  settings.load();
  ui::applySettings();
  if (timeChanged) net::applyTime();
  if (alertsChanged) alerts::reload();
  if (weatherChanged) weather::reload();
  if (outageChanged) outagescreen::reload();
  ok();
}

// ---- files --------------------------------------------------------------------------

static String joinPath(String dir, const String &name) {
  if (!dir.endsWith("/")) dir += "/";
  return dir + name;
}

// Same HTML table as the stock firmware; the web UI parses it.
static void handleFileList() {
  String dir = server.arg("dir");
  File d = LittleFS.open(dir.isEmpty() ? "/" : dir, "r");
  if (!d || !d.isDirectory()) return server.send(200, "text/html", "failed o dir");
  String h = F("<table id='list'><tbody><tr><th>#</th><th>File name</th><th>Size(KB)</th><th></th></tr>");
  int n = 0;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (f.isDirectory()) continue;
    String path = joinPath(dir, f.name());
    h += String(F("<tr><td>")) + (++n) + F("</td><td><a href='") + path + F("'>") + f.name() + F("</a></td><td>") +
         (f.size() / 1024) + F("</td><td><input type='button' class='btndel' onclick=\"deletef('") + path +
         F("')\" value='X'></td></tr>");
  }
  h += "</tbody></table>";
  server.send(200, "text/html", h);
}

static File uploadFile;
static bool uploadOk;

static void handleUploadData() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    String dir = server.arg("dir");
    if (dir.isEmpty()) dir = "/image";
    if (!LittleFS.exists(dir)) LittleFS.mkdir(dir);
    String name = u.filename.substring(u.filename.lastIndexOf('/') + 1);
    name.replace("&", "_");
    uploadOk = name.length() > 0;
    if (uploadOk) uploadFile = LittleFS.open(joinPath(dir, name), "w");
    uploadOk = uploadOk && uploadFile;
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (uploadOk && uploadFile.write(u.buf, u.currentSize) != u.currentSize) uploadOk = false;
  } else if (u.status == UPLOAD_FILE_END || u.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
    if (u.status == UPLOAD_FILE_ABORTED) uploadOk = false;
  }
}

static void handleDelete() {
  String path = server.arg("file");
  if (path.isEmpty() || path == "/.sys/config.json" || !LittleFS.exists(path))
    return server.send(400, "text/plain", "bad file");
  LittleFS.remove(path) ? ok() : server.send(500, "text/plain", "delete failed");
}

static void handleStatic() {
  String path = urlDecode(server.uri());
  if (LittleFS.exists(path)) {
    File f = LittleFS.open(path, "r");
    if (f && !f.isDirectory()) {
      server.sendHeader("Cache-Control", path.startsWith("/.sys/") ? "no-store" : "max-age=3600");
      server.streamFile(f, contentType(path));
      f.close();
      return;
    }
  }
  if (net::apActive()) {  // captive portal: send every unknown URL to the UI
    server.sendHeader("Location", "http://" + net::ip() + "/");
    return server.send(302, "text/plain", "");
  }
  server.send(404, "text/plain", "Not found");
}

// ---- OTA, flash ---------------------------------------------------------------------

static const char UPDATE_PAGE[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>UASmartBox</title></head><body style='font-family:sans-serif'><h3>UASmartBox — оновлення прошивки</h3>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='firmware' accept='.bin'> <input type='submit' value='Update'></form></body></html>";

// An ESP32 image also starts with 0xE9 and the ESP8266 updater would take it (and the device would
// not boot again): ESP32 apps carry the app descriptor magic 0xABCD5432 at offset 0x20.
static bool wrongChip = false;
static bool isEsp32Image(const uint8_t *b, size_t n) {
  return n >= 0x24 && b[0] == 0xE9 && b[0x20] == 0x32 && b[0x21] == 0x54 && b[0x22] == 0xCD && b[0x23] == 0xAB;
}

static void handleUpdateData() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    wrongChip = false;
    alerts::pause(true);  // no fetches between the upload and the reboot
    weather::pause(true);
    outagescreen::pause(true);
    ui::message("Оновлення", "не вимикайте живлення");
    uint32_t maxSketch = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (!Update.begin(maxSketch, U_FLASH)) Update.printError(Serial);
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (Update.progress() == 0 && isEsp32Image(u.buf, u.currentSize)) {
      wrongChip = true;
      Update.end(false);
    }
    if (wrongChip) return;
    if (Update.write(u.buf, u.currentSize) != u.currentSize) Update.printError(Serial);
  } else if (u.status == UPLOAD_FILE_END) {
    if (wrongChip) return;
    if (!Update.end(true)) Update.printError(Serial);
  } else if (u.status == UPLOAD_FILE_ABORTED) {  // connection lost: back to the apps
    Update.end(false);
    alerts::pause(false);
    weather::pause(false);
    outagescreen::pause(false);
    ui::applySettings();
  }
}

static void handleUpdateDone() {
  bool success = !wrongChip && !Update.hasError() && Update.isFinished();
  server.sendHeader("Connection", "close");
  if (!success) {
    alerts::pause(false);
    weather::pause(false);
    outagescreen::pause(false);
    server.send(500, "text/plain",
                wrongChip ? String(F("FAIL: this is an ESP32 (SmallTV Pro) firmware, use UASmartBox-SD-*.bin"))
                          : String("FAIL: ") + Update.getErrorString());
    ui::applySettings();
    return;
  }
  server.send(200, "text/html", F("<META http-equiv=\"refresh\" content=\"15;URL=/\">Update Success! Rebooting..."));
  restartSoon("Оновлено");
}

static void handleFlash() {
  uint32_t chip = ESP.getFlashChipRealSize();
  uint32_t off = server.hasArg("offset") ? strtoul(server.arg("offset").c_str(), NULL, 0) : 0;
  uint32_t size = server.hasArg("size") ? strtoul(server.arg("size").c_str(), NULL, 0) : chip - off;
  if (off >= chip || size == 0 || off + size > chip) return server.send(400, "text/plain", "bad range");
  std::unique_ptr<uint32_t[]> mem(new (std::nothrow) uint32_t[256]);  // 1 KB, only during the dump
  uint32_t *buf = mem.get();
  if (!buf) return server.send(503, "text/plain", "no memory");
  const size_t BUF = 1024;
  WiFiClient c = server.client();
  server.setContentLength(size);
  server.sendHeader("Content-Disposition", "attachment; filename=flash.bin");
  server.send(200, "application/octet-stream", "");
  for (uint32_t a = off; a < off + size && c.connected();) {
    uint32_t n = min<uint32_t>(BUF, off + size - a);
    if (!ESP.flashRead(a, buf, n) || c.write((const uint8_t *)buf, n) != n) break;
    a += n;
    yield();
  }
}

// ---- setup --------------------------------------------------------------------------


void begin() {
  server.on("/", HTTP_GET, [] {
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html", (const char *)WEB_INDEX_GZ, WEB_INDEX_GZ_LEN);
  });
  server.on("/set", HTTP_GET, handleSet);
  server.on("/v.json", HTTP_GET, [] {
    server.send(200, "application/json", String("{\"m\":\"") + MODEL_NAME + "\",\"v\":\"UA Smart Box " FW_VERSION "\"}");
  });
  server.on("/space.json", HTTP_GET, [] {
    FSInfo fi;
    LittleFS.info(fi);
    server.send(200, "application/json", String("{\"total\":") + fi.totalBytes + ",\"free\":" + (fi.totalBytes - fi.usedBytes) + "}");
  });
  server.on("/wifi.json", HTTP_GET, [] { server.send(200, "application/json", net::scanJson()); });
  server.on("/wifisave", HTTP_GET, [] {
    if (!server.hasArg("s") || server.arg("s").isEmpty()) return server.send(400, "text/plain", "ssid required");
    net::saveWifi(server.arg("s"), server.arg("p"));
    server.send(200, "text/plain", "saved");
    restartSoon("Wi-Fi збережено");
  });
  server.on("/wifiknown.json", HTTP_GET, [] { server.send(200, "application/json", net::knownJson()); });
  server.on("/wififorget", HTTP_GET, [] {
    net::forgetWifi(server.arg("s"));
    ok();
  });
  server.on("/filelist", HTTP_GET, handleFileList);
  server.on(
      "/doUpload", HTTP_POST,
      [] { uploadOk ? ok() : server.send(500, "text/plain", "upload failed"); },
      handleUploadData);
  server.on("/delete", HTTP_GET, handleDelete);
  server.on("/update", HTTP_GET, [] { server.send_P(200, "text/html", UPDATE_PAGE); });
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateData);
  server.on("/flash", HTTP_GET, handleFlash);
  server.on("/alerts.json", HTTP_GET, [] { server.send(200, "application/json", alerts::statusJson()); });
  server.on("/outage.json", HTTP_GET, [] { server.send(200, "application/json", outagescreen::statusJson()); });
  server.on("/sys.json", HTTP_GET, [] { server.send(200, "application/json", diag::json()); });
  server.on("/weather.json", HTTP_GET, [] { server.send(200, "application/json", weather::statusJson()); });
  server.on("/state.json", HTTP_GET, [] { server.send(200, "application/json", ui::stateJson()); });
  server.on("/alerts/test", HTTP_GET, [] {
    alerts::testAlarm(server.arg("level").toInt() == 1 ? alerts::PARTIAL : alerts::FULL);
    ui::open("Alerts") ? ok() : server.send(400, "text/plain", "alerts app disabled");
  });
  server.onNotFound(handleStatic);
  server.begin();
}

// Called from loop(); handlers run on the same thread as the screen, so they may draw.
void loop() { server.handleClient(); }

}  // namespace web
