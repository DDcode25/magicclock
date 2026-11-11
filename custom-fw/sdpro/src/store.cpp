#include "store.h"
#include <LittleFS.h>
#include "apps.h"

Settings settings;

namespace store {

static bool fsMounted = false;

// The file system is formatted when it does not mount: the first boot after the stock firmware finds
// its old data (different layout) there. Settings are small and the web UI recreates them.
bool begin() {
  fsMounted = LittleFS.begin();
  if (fsMounted && !LittleFS.exists(F("/.sys"))) LittleFS.mkdir(F("/.sys"));
  return fsMounted;
}

bool mounted() { return fsMounted; }

JsonDocument load(PGM_P path) {
  JsonDocument doc;
  if (fsMounted) {
    File f = LittleFS.open(FPSTR(path), "r");
    if (f) {
      if (deserializeJson(doc, f)) doc.clear();
      f.close();
    }
  }
  if (!doc.is<JsonObject>()) doc.to<JsonObject>();
  return doc;
}

bool save(PGM_P path, const JsonDocument &doc) {
  if (!fsMounted) return false;
  File f = LittleFS.open(FPSTR(path), "w");
  if (!f) return false;
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}

int asInt(JsonVariantConst v, int def) {
  if (v.is<int>()) return v.as<int>();
  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    char *end;
    long n = strtol(s, &end, 10);
    return end != s ? (int)n : def;
  }
  return def;
}

int getInt(PGM_P path, PGM_P key, int def) {
  JsonDocument doc = load(path);
  return asInt(doc[FPSTR(key)], def);
}

String getStr(PGM_P path, PGM_P key, PGM_P def) {
  JsonDocument doc = load(path);
  JsonVariantConst v = doc[FPSTR(key)];
  if (v.isNull()) return FPSTR(def);
  return v.is<const char *>() ? String(v.as<const char *>()) : v.as<String>();
}

}  // namespace store

// Brightness is stored inverted (brt = 255 - percent), as the stock UI expects.
static int pctFromStored(int stored) { return constrain(255 - stored, 0, 100); }

void Settings::load() {
  brightness = pctFromStored(store::getInt(PSTR("/.sys/brt.json"), PSTR("brt"), 205));

  nightEnabled = store::getInt(PSTR("/.sys/timebrt.json"), PSTR("en"), 0) == 1;
  nightFrom = store::getInt(PSTR("/.sys/timebrt.json"), PSTR("t1"), 22);
  nightTo = store::getInt(PSTR("/.sys/timebrt.json"), PSTR("t2"), 7);
  nightBrightness = pctFromStored(store::getInt(PSTR("/.sys/timebrt.json"), PSTR("b2"), 205));

  JsonDocument app = store::load(PSTR("/.sys/app.json"));
  bootApp = store::getInt(PSTR("/.sys/app.json"), PSTR("theme"), -1);
  JsonObjectConst flags = app["apps"];
  for (int i = 0; i < APP_COUNT; i++) {
    JsonVariantConst f = flags[appKey(i)];
    // No touch on this device: the Settings screen is off unless switched on (the IP shows at boot).
    bool def = i != APP_SETTINGS;
    appEnabled[i] = appAvailable(i) &&
                    (f.isNull() ? def : f.as<int>() == 1 || (f.is<const char *>() && strcmp(f.as<const char *>(), "1") == 0));
  }
  autoNext = store::asInt(app["auto"], 1) != 0;
  JsonObjectConst dur = app["dur"];
  for (int i = 0; i < APP_COUNT; i++) appSec[i] = constrain(store::asInt(dur[appKey(i)], 15), 0, 3600);

  tzManual = store::getInt(PSTR("/.sys/tz.json"), PSTR("mode"), 0) == 1;
  tzOffsetMin = store::getInt(PSTR("/.sys/tz.json"), PSTR("offset"), 0);
  ntpServer = store::getStr(PSTR("/.sys/ntp.json"), PSTR("ntp"), PSTR(""));
  if (ntpServer.isEmpty()) ntpServer = F("pool.ntp.org");
}
