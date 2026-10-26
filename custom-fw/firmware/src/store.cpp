#include "store.h"
#include <LittleFS.h>
#include "apps.h"

Settings settings;

namespace store {

static bool fsMounted = false;

bool begin() {
  fsMounted = LittleFS.begin(false, "/littlefs", 10, "spiffs");
  if (fsMounted && !LittleFS.exists("/.sys")) LittleFS.mkdir("/.sys");
  return fsMounted;
}

bool mounted() { return fsMounted; }

JsonDocument load(const char *path) {
  JsonDocument doc;
  if (fsMounted) {
    File f = LittleFS.open(path, "r");
    if (f) {
      if (deserializeJson(doc, f)) doc.clear();
      f.close();
    }
  }
  if (!doc.is<JsonObject>()) doc.to<JsonObject>();
  return doc;
}

bool save(const char *path, const JsonDocument &doc) {
  if (!fsMounted) return false;
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}

static int asInt(JsonVariantConst v, int def) {
  if (v.is<int>()) return v.as<int>();
  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    char *end;
    long n = strtol(s, &end, 10);
    return end != s ? (int)n : def;
  }
  return def;
}

int getInt(const char *path, const char *key, int def) {
  JsonDocument doc = load(path);
  return asInt(doc[key], def);
}

String getStr(const char *path, const char *key, const char *def) {
  JsonDocument doc = load(path);
  JsonVariantConst v = doc[key];
  if (v.isNull()) return def;
  return v.is<const char *>() ? String(v.as<const char *>()) : v.as<String>();
}

}  // namespace store

// Brightness is stored inverted (brt = 255 - percent), as the stock UI expects.
static int pctFromStored(int stored) { return constrain(255 - stored, 0, 100); }

void Settings::load() {
  brightness = pctFromStored(store::getInt("/.sys/brt.json", "brt", 205));

  nightEnabled = store::getInt("/.sys/timebrt.json", "en", 0) == 1;
  nightFrom = store::getInt("/.sys/timebrt.json", "t1", 22);
  nightTo = store::getInt("/.sys/timebrt.json", "t2", 7);
  nightBrightness = pctFromStored(store::getInt("/.sys/timebrt.json", "b2", 205));

  JsonDocument app = store::load("/.sys/app.json");
  bootApp = store::getInt("/.sys/app.json", "theme", -1);
  JsonObjectConst flags = app["apps"];
  for (int i = 0; i < APP_COUNT; i++) {
    JsonVariantConst f = flags[APP_ITEMS[i].key];
    appEnabled[i] = f.isNull() || f.as<int>() == 1 || (f.is<const char *>() && strcmp(f.as<const char *>(), "1") == 0);
  }

  tzManual = store::getInt("/.sys/tz.json", "mode", 0) == 1;
  tzOffsetMin = store::getInt("/.sys/tz.json", "offset", 0);
  ntpServer = store::getStr("/.sys/ntp.json", "ntp", "");
  if (ntpServer.isEmpty()) ntpServer = "pool.ntp.org";
}
