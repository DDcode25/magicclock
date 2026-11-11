#include "fw_version.h"
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

// Strings live in flash (PSTR / F / PROGMEM): RAM is the scarce resource on the ESP8266. The server
// reads content types and send_P bodies from flash itself.
static const char TEXT[] PROGMEM = "text/plain";
static const char HTML[] PROGMEM = "text/html";
static const char JSON_TYPE[] PROGMEM = "application/json";

static void reply(int code, PGM_P msg) { server.send_P(code, TEXT, msg); }
static void ok() { reply(200, PSTR("OK")); }
static void sendJson(const String &body) { server.send(200, JSON_TYPE, body); }

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

static String contentType(const String &path) {
  if (path.endsWith(F(".json"))) return FPSTR(JSON_TYPE);
  if (path.endsWith(F(".gif"))) return F("image/gif");
  if (path.endsWith(F(".jpg")) || path.endsWith(F(".jpeg"))) return F("image/jpeg");
  if (path.endsWith(F(".png"))) return F("image/png");
  if (path.endsWith(F(".ico"))) return F("image/x-icon");
  if (path.endsWith(F(".html")) || path.endsWith(F(".htm"))) return FPSTR(HTML);
  if (path.endsWith(F(".css"))) return F("text/css");
  if (path.endsWith(F(".js"))) return F("application/javascript");
  return F("application/octet-stream");
}

// ---- /set ---------------------------------------------------------------------------

enum Kind { STR, INT, INV_STR, INV_INT };  // INV: stored as 255 - percent (brightness)

// In flash (PROGMEM, read with memcpy_P); file is the name in /.sys without ".json".
struct Field {
  char param[10], file[10], key[9];
  uint8_t kind;
};

// SD PRO: only the settings of the apps built here (the stock UI's crypto, stock, album and monitor
// fields are ignored).
static const Field FIELDS[] PROGMEM = {
    {"cd1", "city", "cd1", STR},
    {"cd2", "city", "cd2", STR},
    {"nickname", "city", "nickname", STR},
    {"interval", "city", "w_i", INT},
    {"w_u", "unit", "w_u", STR},
    {"t_u", "unit", "t_u", STR},
    {"p_u", "unit", "p_u", STR},
    {"hc", "timecolor", "hc", STR},
    {"mc", "timecolor", "mc", STR},
    {"sc", "timecolor", "sc", STR},
    {"hour", "hour12", "h", STR},
    {"day", "day", "day", STR},
    {"sec_mode", "clock", "sm", INT},
    {"tz_offset", "tz", "offset", INT},
    {"ntp", "ntp", "ntp", STR},
    {"brt", "brt", "brt", INV_STR},
    {"t1", "timebrt", "t1", INT},
    {"t2", "timebrt", "t2", INT},
    {"b1", "timebrt", "b1", INT},
    {"b2", "timebrt", "b2", INV_INT},
    {"en", "timebrt", "en", INT},
    {"theme", "app", "theme", STR},
    // UA Smart Box: air alerts app
    {"al_src", "alerts", "src", STR},
    {"al_token", "alerts", "token", STR},
    {"al_i", "alerts", "i", INT},
    {"al_home", "alerts", "home", INT},
    {"al_cc", "alerts", "cc", STR},
    {"al_auto", "alerts", "auto", INT},
    // UA Smart Box: power outage schedule
    {"out_src", "outage", "src", STR},
    {"out_grp", "outage", "grp", STR},
    {"out_i", "outage", "i", INT},
    {"out_hc", "outage", "hc", STR},
    {"out_sq", "outage", "sq", INT},
};

class Batch {
 public:
  // file: name in /.sys without ".json"
  JsonDocument &doc(const String &file) {
    auto it = docs.find(file);
    if (it == docs.end()) it = docs.emplace(file, store::load(path(file).c_str())).first;
    return it->second;
  }
  void save() {
    for (auto &kv : docs) store::save(path(kv.first).c_str(), kv.second);
  }

 private:
  std::map<String, JsonDocument> docs;
  static String path(const String &file) { return String(F("/.sys/")) + file + F(".json"); }
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

static bool clearDir(PGM_P dirP) {
  String dir = FPSTR(dirP);
  File d = LittleFS.open(dir, "r");
  if (!d || !d.isDirectory()) return false;
  std::vector<String> files;
  for (File f = d.openNextFile(); f; f = d.openNextFile())
    if (!f.isDirectory()) files.push_back(dir + '/' + f.name());
  for (auto &p : files) LittleFS.remove(p);
  return true;
}

static void restartSoon(const __FlashStringHelper *why) {
  ui::message(why, F("перезапуск…"));
  delay(800);
  ESP.restart();
}

static void handleSet() {
  diag::crumb(diag::WEB, PSTR("web /set"));
  // Actions first.
  if (server.hasArg(F("page"))) {
    ui::next(server.arg(F("page")).toInt());
    return ok();
  }
  if (server.hasArg(F("enter"))) {
    ui::enter();
    return ok();
  }
  if (server.hasArg(F("open_app"))) {
    return ui::open(server.arg(F("open_app"))) ? ok() : reply(400, PSTR("app disabled or unknown"));
  }
  if (server.hasArg(F("reboot"))) {
    ok();
    restartSoon(F("Перезапуск"));
  }
  if (server.hasArg(F("reset"))) {
    // Factory reset of settings; Wi-Fi (config.json) is kept so the device stays reachable.
    ok();
    File d = LittleFS.open(F("/.sys"), "r");
    std::vector<String> files;
    for (File f = d.openNextFile(); f; f = d.openNextFile())
      if (strcmp_P(f.name(), PSTR("config.json"))) files.push_back(String(F("/.sys/")) + f.name());
    for (auto &p : files) LittleFS.remove(p);
    restartSoon(F("Скидання"));
  }
  if (server.hasArg(F("clear"))) {
    String kind = server.arg(F("clear"));
    bool done = kind.equals(F("gif"))     ? clearDir(PSTR("/image/80x80.gif"))
                : kind.equals(F("image")) ? clearDir(PSTR("/image"))
                                          : false;
    return done ? ok() : reply(400, PSTR("bad clear"));
  }

  Batch batch;
  bool timeChanged = false, alertsChanged = false, weatherChanged = false, outageChanged = false;
  for (int i = 0; i < server.args(); i++) {
    String name = server.argName(i), v = server.arg(i);
    if (name.equals(F("plain"))) continue;
    bool handled = false;
    for (const Field &pf : FIELDS) {
      Field f;
      memcpy_P(&f, &pf, sizeof f);
      if (name == f.param) {
        setValue(batch.doc(f.file), f.key, v, (Kind)f.kind);
        if (!strcmp_P(f.file, PSTR("outage"))) outageChanged = true;
        if (!strcmp_P(f.file, PSTR("city")) || !strcmp_P(f.file, PSTR("unit"))) weatherChanged = true;
        handled = true;
        break;
      }
    }
    if (handled) {
      if (name.equals(F("cd1"))) batch.doc(F("city"))[F("ct1")] = v;
      if (name.equals(F("tz_offset")) || name.equals(F("ntp"))) timeChanged = true;
      if (name.startsWith(F("al_"))) alertsChanged = true;
      continue;
    }
    if (name.equals(F("tz_mode"))) {
      batch.doc(F("tz"))[F("mode")] = v.equals(F("manual")) || v.equals(F("1")) ? 1 : 0;
      timeChanged = true;
    } else if (name.startsWith(F("app_"))) {
      batch.doc(F("app"))[F("apps")][name.substring(4)] = v.toInt();
    } else if (name.startsWith(F("dur_"))) {  // seconds on screen per app (0 = stays)
      batch.doc(F("app"))[F("dur")][name.substring(4)] = constrain(v.toInt(), 0, 3600);
    } else if (name.equals(F("auto"))) {  // auto rotation on / off
      batch.doc(F("app"))[F("auto")] = v.toInt() ? 1 : 0;
    } else {
      Serial.printf_P(PSTR("set: unknown %s=%s\n"), name.c_str(), v.c_str());
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
  if (!dir.endsWith(F("/"))) dir += '/';
  return dir + name;
}

// Same HTML table as the stock firmware; the web UI parses it.
static void handleFileList() {
  String dir = server.arg(F("dir"));
  File d = LittleFS.open(dir.isEmpty() ? String('/') : dir, "r");
  if (!d || !d.isDirectory()) return server.send_P(200, HTML, PSTR("failed o dir"));
  String h = F("<table id='list'><tbody><tr><th>#</th><th>File name</th><th>Size(KB)</th><th></th></tr>");
  int n = 0;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (f.isDirectory()) continue;
    String path = joinPath(dir, f.name());
    h += String(F("<tr><td>")) + (++n) + F("</td><td><a href='") + path + F("'>") + f.name() + F("</a></td><td>") +
         (f.size() / 1024) + F("</td><td><input type='button' class='btndel' onclick=\"deletef('") + path +
         F("')\" value='X'></td></tr>");
  }
  h += F("</tbody></table>");
  server.send(200, HTML, h);
}

static File uploadFile;
static bool uploadOk;

static void handleUploadData() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    String dir = server.arg(F("dir"));
    if (dir.isEmpty()) dir = F("/image");
    if (!LittleFS.exists(dir)) LittleFS.mkdir(dir);
    String name = u.filename.substring(u.filename.lastIndexOf('/') + 1);
    name.replace('&', '_');
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
  String path = server.arg(F("file"));
  if (path.isEmpty() || path.equals(F("/.sys/config.json")) || !LittleFS.exists(path))
    return reply(400, PSTR("bad file"));
  LittleFS.remove(path) ? ok() : reply(500, PSTR("delete failed"));
}

static void handleStatic() {
  String path = urlDecode(server.uri());
  if (LittleFS.exists(path)) {
    File f = LittleFS.open(path, "r");
    if (f && !f.isDirectory()) {
      server.sendHeader(F("Cache-Control"), path.startsWith(F("/.sys/")) ? F("no-store") : F("max-age=3600"));
      server.streamFile(f, contentType(path));
      f.close();
      return;
    }
  }
  if (net::apActive()) {  // captive portal: send every unknown URL to the UI
    server.sendHeader(F("Location"), String(F("http://")) + net::ip() + '/');
    return reply(302, PSTR(""));
  }
  reply(404, PSTR("Not found"));
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
    ui::message(F("Оновлення"), F("не вимикайте живлення"));
    ui::progress(0);
    uint32_t maxSketch = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (!Update.begin(maxSketch, U_FLASH)) Update.printError(Serial);
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (Update.progress() == 0 && isEsp32Image(u.buf, u.currentSize)) {
      wrongChip = true;
      Update.end(false);
    }
    if (wrongChip) return;
    if (Update.write(u.buf, u.currentSize) != u.currentSize) Update.printError(Serial);
    if (u.contentLength) ui::progress((uint64_t)u.totalSize * 100 / u.contentLength);
  } else if (u.status == UPLOAD_FILE_END) {
    if (wrongChip) return;
    if (!Update.end(true)) Update.printError(Serial);
    ui::progress(100);
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
  server.sendHeader(F("Connection"), F("close"));
  if (!success) {
    alerts::pause(false);
    weather::pause(false);
    outagescreen::pause(false);
    server.send(500, TEXT,
                wrongChip ? String(F("FAIL: this is an ESP32 (SmallTV Pro) firmware, use UASmartBox-SD-*.bin"))
                          : String(F("FAIL: ")) + Update.getErrorString());
    ui::applySettings();
    return;
  }
  server.send_P(200, HTML, PSTR("<META http-equiv=\"refresh\" content=\"15;URL=/\">Update Success! Rebooting..."));
  restartSoon(F("Оновлено"));
}

static void handleFlash() {
  uint32_t chip = ESP.getFlashChipRealSize();
  uint32_t off = server.hasArg(F("offset")) ? strtoul(server.arg(F("offset")).c_str(), NULL, 0) : 0;
  uint32_t size = server.hasArg(F("size")) ? strtoul(server.arg(F("size")).c_str(), NULL, 0) : chip - off;
  if (off >= chip || size == 0 || off + size > chip) return reply(400, PSTR("bad range"));
  std::unique_ptr<uint32_t[]> mem(new (std::nothrow) uint32_t[256]);  // 1 KB, only during the dump
  uint32_t *buf = mem.get();
  if (!buf) return reply(503, PSTR("no memory"));
  const size_t BUF = 1024;
  WiFiClient c = server.client();
  server.setContentLength(size);
  server.sendHeader(F("Content-Disposition"), F("attachment; filename=flash.bin"));
  server.send_P(200, PSTR("application/octet-stream"), PSTR(""));
  for (uint32_t a = off; a < off + size && c.connected();) {
    uint32_t n = min<uint32_t>(BUF, off + size - a);
    if (!ESP.flashRead(a, buf, n) || c.write((const uint8_t *)buf, n) != n) break;
    a += n;
    yield();
  }
}

// ---- setup --------------------------------------------------------------------------


void begin() {
  server.on(F("/"), HTTP_GET, [] {
    server.sendHeader(F("Content-Encoding"), F("gzip"));
    server.send_P(200, HTML, (const char *)WEB_INDEX_GZ, WEB_INDEX_GZ_LEN);
  });
  server.on(F("/set"), HTTP_GET, handleSet);
  server.on(F("/v.json"), HTTP_GET, [] {
    server.send_P(200, JSON_TYPE, PSTR("{\"m\":\"" MODEL_NAME "\",\"v\":\"UA Smart Box " FW_VERSION "\"}"));
  });
  server.on(F("/space.json"), HTTP_GET, [] {
    FSInfo fi;
    LittleFS.info(fi);
    char b[48];
    snprintf_P(b, sizeof b, PSTR("{\"total\":%u,\"free\":%u}"), (unsigned)fi.totalBytes, (unsigned)(fi.totalBytes - fi.usedBytes));
    sendJson(b);
  });
  server.on(F("/wifi.json"), HTTP_GET, [] { sendJson(net::scanJson()); });
  server.on(F("/wifisave"), HTTP_GET, [] {
    if (!server.hasArg(F("s")) || server.arg(F("s")).isEmpty()) return reply(400, PSTR("ssid required"));
    net::saveWifi(server.arg(F("s")), server.arg(F("p")));
    reply(200, PSTR("saved"));
    restartSoon(F("Wi-Fi збережено"));
  });
  server.on(F("/wifiknown.json"), HTTP_GET, [] { sendJson(net::knownJson()); });
  server.on(F("/wififorget"), HTTP_GET, [] {
    net::forgetWifi(server.arg(F("s")));
    ok();
  });
  server.on(F("/filelist"), HTTP_GET, handleFileList);
  server.on(F("/doUpload"), HTTP_POST,
      [] { uploadOk ? ok() : reply(500, PSTR("upload failed")); },
      handleUploadData);
  server.on(F("/delete"), HTTP_GET, handleDelete);
  server.on(F("/update"), HTTP_GET, [] { server.send_P(200, HTML, UPDATE_PAGE); });
  server.on(F("/update"), HTTP_POST, handleUpdateDone, handleUpdateData);
  server.on(F("/flash"), HTTP_GET, handleFlash);
  server.on(F("/alerts.json"), HTTP_GET, [] { sendJson(alerts::statusJson()); });
  server.on(F("/outage.json"), HTTP_GET, [] { sendJson(outagescreen::statusJson()); });
  server.on(F("/sys.json"), HTTP_GET, [] {
    if (server.hasArg(F("reset"))) diag::resetLow();
    sendJson(diag::json());
  });
  server.on(F("/weather.json"), HTTP_GET, [] { sendJson(weather::statusJson()); });
  server.on(F("/state.json"), HTTP_GET, [] { sendJson(ui::stateJson()); });
  server.on(F("/alerts/test"), HTTP_GET, [] {
    alerts::testAlarm(server.arg(F("level")).toInt() == 1 ? alerts::PARTIAL : alerts::FULL);
    ui::open(F("Alerts")) ? ok() : reply(400, PSTR("alerts app disabled"));
  });
  server.onNotFound(handleStatic);
  server.begin();
}

// Called from loop(); handlers run on the same thread as the screen, so they may draw.
void loop() { server.handleClient(); }

}  // namespace web
