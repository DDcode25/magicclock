#include "web.h"
#include <WebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <esp_flash.h>
#include <map>
#include "apps.h"
#include "board.h"
#include "net.h"
#include "store.h"
#include "ui.h"
#include "web_index.h"

namespace web {

static WebServer server(80);

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

static const Field FIELDS[] = {
    {"c_theme", "c_theme.json", "c_theme", STR},
    {"c_chart", "c_chart.json", "c_chart", STR},
    {"kline", "k.json", "k", STR},
    {"c_loading", "coin2.json", "c_loading", INT},
    {"c_clock", "coin2.json", "c_clock", INT},
    {"ani", "coin2.json", "c_ani", INT},
    {"c_pct_mode", "coin2.json", "c_pct_mode", INT},
    {"c_i", "coin2.json", "c_i", INT},
    {"c_l", "coin2.json", "c_l", INT},
    {"rot_i", "cr.json", "rot_i", STR},
    {"g_theme", "g_theme.json", "g_theme", STR},
    {"g_range", "cg_range.json", "range", STR},
    {"g_loading", "cgk.json", "g_loading", INT},
    {"g_clock", "cgk.json", "g_clock", INT},
    {"g_ani", "cgk.json", "g_ani", INT},
    {"g_i", "cgk.json", "g_i", INT},
    {"g_l", "cgk.json", "g_l", INT},
    {"g_rot_i", "cgk.json", "g_rot_i", INT},
    {"st_chart", "st_chart.json", "st_chart", STR},
    {"st_kline", "stk.json", "st_kline", STR},
    {"s_i", "stock2.json", "s_i", STR},
    {"s_l", "stock2.json", "s_l", STR},
    {"s_ani", "stock2.json", "s_ani", STR},
    {"cd1", "city.json", "cd1", STR},
    {"cd2", "city.json", "cd2", STR},
    {"nickname", "city.json", "nickname", STR},
    {"interval", "city.json", "w_i", INT},
    {"w_u", "unit.json", "w_u", STR},
    {"t_u", "unit.json", "t_u", STR},
    {"p_u", "unit.json", "p_u", STR},
    {"key", "key.json", "key", STR},
    {"hc", "timecolor.json", "hc", STR},
    {"mc", "timecolor.json", "mc", STR},
    {"sc", "timecolor.json", "sc", STR},
    {"hour", "hour12.json", "h", STR},
    {"day", "day.json", "day", STR},
    {"tz_offset", "tz.json", "offset", INT},
    {"ntp", "ntp.json", "ntp", STR},
    {"gif_loop", "album.json", "gif_loop", INT},
    {"i_i", "album.json", "i_i", INT},
    {"autoplay", "album.json", "autoplay", INT},
    {"album_path", "album.json", "path", STR},
    {"album_autoplay", "album.json", "autoplay", INT},
    {"gif", "gif.json", "gif", STR},
    {"ip", "monitor.json", "ip", STR},
    {"m_i", "monitor.json", "m_i", INT},
    {"delay", "delay.json", "delay", STR},
    {"brt", "brt.json", "brt", INV_STR},
    {"t1", "timebrt.json", "t1", INT},
    {"t2", "timebrt.json", "t2", INT},
    {"b1", "timebrt.json", "b1", INT},
    {"b2", "timebrt.json", "b2", INV_INT},
    {"en", "timebrt.json", "en", INT},
    {"theme", "app.json", "theme", STR},
    {"up_fg", "ccolor.json", "up_fg", STR},
    {"up_bg", "ccolor.json", "up_bg", STR},
    {"down_fg", "ccolor.json", "down_fg", STR},
    {"down_bg", "ccolor.json", "down_bg", STR},
    {"coin_fg", "ccolor.json", "coin_fg", STR},
    {"coin_bg", "ccolor.json", "coin_bg", STR},
    {"pct_up_fg", "ccolor.json", "pct_up_fg", STR},
    {"pct_up_bg", "ccolor.json", "pct_up_bg", STR},
    {"pct_down_fg", "ccolor.json", "pct_down_fg", STR},
    {"pct_down_bg", "ccolor.json", "pct_down_bg", STR},
    {"dec_sep", "sep.json", "decimal_sep", STR},
    {"thous_sep", "sep.json", "thousand_sep", STR},
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

static void setValue(JsonVariant dst, const String &v, Kind kind) {
  switch (kind) {
    case STR: dst.set(v); break;
    case INT: dst.set(v.toInt()); break;
    case INV_STR: dst.set(String(255 - constrain(v.toInt(), 0, 100))); break;
    case INV_INT: dst.set(255 - constrain(v.toInt(), 0, 100)); break;
  }
}

static bool clearDir(const char *dir) {
  File d = LittleFS.open(dir);
  if (!d || !d.isDirectory()) return false;
  std::vector<String> files;
  for (File f = d.openNextFile(); f; f = d.openNextFile())
    if (!f.isDirectory()) files.push_back(String(dir) + "/" + f.name());
  for (auto &p : files) LittleFS.remove(p);
  return true;
}

static void restartSoon(const char *why) {
  ui::message(why, "rebooting...");
  delay(800);
  ESP.restart();
}

static void handleSet() {
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
    restartSoon("Reboot");
  }
  if (server.hasArg("reset")) {
    // Factory reset of settings; Wi-Fi (config.json) is kept so the device stays reachable.
    ok();
    File d = LittleFS.open("/.sys");
    std::vector<String> files;
    for (File f = d.openNextFile(); f; f = d.openNextFile())
      if (strcmp(f.name(), "config.json")) files.push_back(String("/.sys/") + f.name());
    for (auto &p : files) LittleFS.remove(p);
    restartSoon("Factory reset");
  }
  if (server.hasArg("clear")) {
    String kind = server.arg("clear");
    bool done = kind == "gif" ? clearDir("/image/80x80.gif") : kind == "image" ? clearDir("/image") : false;
    return done ? ok() : server.send(400, "text/plain", "bad clear");
  }

  Batch batch;
  bool stockForm = server.hasArg("s_i");  // cn0..9 belongs to stock2.json in the stock form
  bool timeChanged = false;
  for (int i = 0; i < server.args(); i++) {
    String name = server.argName(i), v = server.arg(i);
    if (name == "plain") continue;
    bool handled = false;
    for (const Field &f : FIELDS) {
      if (name == f.param) {
        setValue(batch.doc(f.file)[f.key], v, f.kind);
        handled = true;
        break;
      }
    }
    if (handled) {
      if (name == "cd1") batch.doc("city.json")["ct1"] = v;
      if (name == "tz_offset" || name == "ntp") timeChanged = true;
      continue;
    }
    if (name == "tz_mode") {
      batch.doc("tz.json")["mode"] = v == "manual" || v == "1" ? 1 : 0;
      timeChanged = true;
    } else if (name.startsWith("app_")) {
      batch.doc("app.json")["apps"][name.substring(4)] = v.toInt();
    } else if (indexed(name, "cin", 12)) {
      batch.doc("coin2.json")[name] = v;
    } else if (indexed(name, "gin", 12)) {
      batch.doc("cgk.json")[name] = v;
    } else if (indexed(name, "cn", 12)) {
      batch.doc(stockForm ? "stock2.json" : "coin2.json")[name] = v;
    } else if (indexed(name, "c", 10)) {
      batch.doc("stock2.json")[name] = v;
    } else {
      Serial.printf("set: unknown %s=%s\n", name.c_str(), v.c_str());
    }
  }
  batch.save();
  settings.load();
  ui::applySettings();
  if (timeChanged) net::applyTime();
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
  File d = LittleFS.open(dir.isEmpty() ? "/" : dir);
  if (!d || !d.isDirectory()) return server.send(200, "text/html", "failed o dir");
  String h = "<table id='list'><tbody><tr><th>#</th><th>File name</th><th>Size(KB)</th><th></th></tr>";
  int n = 0;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (f.isDirectory()) continue;
    String path = joinPath(dir, f.name());
    h += "<tr><td>" + String(++n) + "</td><td><a href='" + path + "'>" + f.name() + "</a></td><td>" +
         String(f.size() / 1024) + "</td><td><input type='button' class='btndel' onclick=\"deletef('" + path +
         "')\" value='X'></td></tr>";
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
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Firmware Update</title></head><body style='font-family:sans-serif'><h3>SmallTV Pro firmware update</h3>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='firmware' accept='.bin'> <input type='submit' value='Update'></form></body></html>";

static void handleUpdateData() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    ui::message("Updating", "do not unplug");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) Update.printError(Serial);
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (Update.write(u.buf, u.currentSize) != u.currentSize) Update.printError(Serial);
  } else if (u.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) Update.printError(Serial);
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
  }
}

static void handleUpdateDone() {
  bool success = !Update.hasError();
  server.sendHeader("Connection", "close");
  if (!success) {
    server.send(500, "text/plain", String("FAIL: ") + Update.errorString());
    ui::applySettings();
    return;
  }
  server.send(200, "text/html", "<META http-equiv=\"refresh\" content=\"15;URL=/\">Update Success! Rebooting...");
  restartSoon("Update done");
}

static void handleFlash() {
  uint32_t chip = ESP.getFlashChipSize();
  uint32_t off = server.hasArg("offset") ? strtoul(server.arg("offset").c_str(), NULL, 0) : 0;
  uint32_t size = server.hasArg("size") ? strtoul(server.arg("size").c_str(), NULL, 0) : chip - off;
  if (off >= chip || size == 0 || off + size > chip) return server.send(400, "text/plain", "bad range");
  static uint8_t buf[4096];
  WiFiClient c = server.client();
  server.setContentLength(size);
  server.sendHeader("Content-Disposition", "attachment; filename=flash.bin");
  server.send(200, "application/octet-stream", "");
  for (uint32_t a = off; a < off + size && c.connected();) {
    uint32_t n = min<uint32_t>(sizeof buf, off + size - a);
    if (esp_flash_read(NULL, buf, a, n) != ESP_OK || c.write(buf, n) != n) break;
    a += n;
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
    server.send(200, "application/json", String("{\"m\":\"") + MODEL_NAME + "\",\"v\":\"OpenFW " FW_VERSION "\"}");
  });
  server.on("/space.json", HTTP_GET, [] {
    server.send(200, "application/json",
                String("{\"total\":") + LittleFS.totalBytes() + ",\"free\":" + (LittleFS.totalBytes() - LittleFS.usedBytes()) + "}");
  });
  server.on("/wifi.json", HTTP_GET, [] { server.send(200, "application/json", net::scanJson()); });
  server.on("/wifisave", HTTP_GET, [] {
    if (!server.hasArg("s") || server.arg("s").isEmpty()) return server.send(400, "text/plain", "ssid required");
    net::saveWifi(server.arg("s"), server.arg("p"));
    server.send(200, "text/plain", "saved");
    restartSoon("Wi-Fi saved");
  });
  server.on("/filelist", HTTP_GET, handleFileList);
  server.on(
      "/doUpload", HTTP_POST, [] { uploadOk ? ok() : server.send(500, "text/plain", "upload failed"); }, handleUploadData);
  server.on("/delete", HTTP_GET, handleDelete);
  server.on("/update", HTTP_GET, [] { server.send_P(200, "text/html", UPDATE_PAGE); });
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateData);
  server.on("/flash", HTTP_GET, handleFlash);
  server.onNotFound(handleStatic);
  server.begin();
}

void loop() { server.handleClient(); }

}  // namespace web
