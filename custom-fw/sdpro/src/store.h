#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Settings live in LittleFS /.sys/*.json, in the same format as the stock firmware,
// so the stock web UI and existing user settings keep working.
namespace store {

bool begin();  // mounts LittleFS, formats it when it does not mount
bool mounted();

// Paths, keys and defaults may be in flash (PSTR / PROGMEM) or in RAM: they are read with the _P
// functions, which take both.
JsonDocument load(PGM_P path);  // empty object if missing or invalid
bool save(PGM_P path, const JsonDocument &doc);

// Stock files mix strings and numbers ("brt":"196" vs "en":0); accept both.
int asInt(JsonVariantConst v, int def);  // number or numeric string, else def
int getInt(PGM_P path, PGM_P key, int def);
String getStr(PGM_P path, PGM_P key, PGM_P def);

}  // namespace store

// Parsed settings used by the device side (display, apps, time).
struct Settings {
  int brightness = 50;  // 0..100 %
  bool nightEnabled = false;
  int nightFrom = 22, nightTo = 7;  // hours
  int nightBrightness = 20;         // 0..100 %
  int bootApp = -1;                 // APP_ITEMS index, -1 = none
  bool appEnabled[16];  // indexed like APP_ITEMS; missing flag = enabled
  bool tzManual = false;
  int tzOffsetMin = 0;
  String ntpServer = "pool.ntp.org";
  // Auto rotation: the enabled apps take turns, each for its own number of seconds (0 = stays on it).
  // /.sys/app.json {"auto":1,"dur":{"weather":15,...}}, dur indexed like APP_ITEMS.
  bool autoNext = true;
  int appSec[16];

  Settings() {
    for (bool &e : appEnabled) e = true;
    for (int &d : appSec) d = 15;
  }
  void load();
};

extern Settings settings;
