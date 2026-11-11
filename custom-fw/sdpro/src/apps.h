#pragma once
#include <Arduino.h>

// Same keys, names and indices as APP_ITEMS in the stock web UI (app.json "theme" is the index).
// The table and its strings are in flash (apps.cpp): read through appKey / appName / appLabel.
struct AppItem {
  PGM_P key;    // app.json "apps" flag, /set?app_<key>=
  PGM_P name;   // /set?open_app=<name>
  PGM_P label;  // shown on the device screen (Ukrainian)
};
static const int APP_COUNT = 11;
extern const AppItem APP_ITEMS[APP_COUNT] PROGMEM;
inline const __FlashStringHelper *appKey(int i) { return FPSTR(pgm_read_ptr(&APP_ITEMS[i].key)); }
inline const __FlashStringHelper *appName(int i) { return FPSTR(pgm_read_ptr(&APP_ITEMS[i].name)); }
inline const __FlashStringHelper *appLabel(int i) { return FPSTR(pgm_read_ptr(&APP_ITEMS[i].label)); }

// Apps built into this firmware (SD PRO: weather, clock, alerts, outage, settings). The others keep
// their index so stock settings files and the web UI stay compatible, but are never shown.
static inline bool appAvailable(int i) { return i == 3 || i == 6 || i == 8 || i == 9 || i == 10; }
// Order of the screens (tap = next): indices into APP_ITEMS. Same order in the web UI (tools/ui_patch.py).
static const int APP_ORDER[APP_COUNT] = {3 /*weather*/, 6 /*clock*/, 9 /*alerts*/, 10 /*outage*/, 0 /*bitcoin*/,
                                         2 /*stock*/, 1 /*coingecko*/, 4 /*picture*/, 5 /*monitor*/, 7 /*idea*/,
                                         8 /*settings*/};
static const int APP_BITCOIN = 0;
static const int APP_WEATHER = 3;
static const int APP_PICTURE = 4;
static const int APP_CLOCK = 6;
static const int APP_SETTINGS = 8;
static const int APP_ALERTS = 9;
static const int APP_OUTAGE = 10;
