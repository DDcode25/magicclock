#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Settings live in LittleFS /.sys/*.json, in the same format as the stock firmware,
// so the stock web UI and existing user settings keep working.
namespace store {

bool begin();  // mounts the "spiffs" partition; never formats
bool mounted();

JsonDocument load(const char *path);  // empty object if missing or invalid
bool save(const char *path, const JsonDocument &doc);

// Stock files mix strings and numbers ("brt":"196" vs "en":0); accept both.
int getInt(const char *path, const char *key, int def);
String getStr(const char *path, const char *key, const char *def);

}  // namespace store

// Parsed settings used by the device side (display, apps, time).
struct Settings {
  int brightness = 50;  // 0..100 %
  bool nightEnabled = false;
  int nightFrom = 22, nightTo = 7;  // hours
  int nightBrightness = 20;         // 0..100 %
  int bootApp = -1;                 // APP_ITEMS index, -1 = none
  bool appEnabled[9] = {true, true, true, true, true, true, true, true, true};
  bool tzManual = false;
  int tzOffsetMin = 0;
  String ntpServer = "pool.ntp.org";

  void load();
};

extern Settings settings;
