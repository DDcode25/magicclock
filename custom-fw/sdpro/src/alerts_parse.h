#pragma once
// Parsers for air raid alert sources. Plain C++ (no Arduino) so they can be tested on a PC.
// Regions are indexed in alerts.in.ua IoT order (same as MAP[] in ukraine_map.h),
// raions like RAION[] in ukraine_map.h.
#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

namespace alerts {

static const int REGIONS = 27;
static const int RAIONS = 136;
enum Level : uint8_t { NONE = 0, PARTIAL = 1, FULL = 2, UNKNOWN = 3 };  // UNKNOWN: region absent from source
// Per-raion alert level as on alerts.in.ua: red = missiles/mass strike, yellow = drones.
// R_INHERIT: the source has no raion data; draw the oblast level instead.
enum RaionLevel : uint8_t { R_NONE = 0, R_YELLOW = 1, R_RED = 2, R_INHERIT = 3 };

struct Snapshot {
  uint8_t level[REGIONS];
  time_t since[REGIONS];  // UTC start of the alert, 0 if unknown
  uint8_t raion[RAIONS];  // RaionLevel
};

extern const char *lastError;  // why the last parse failed (diagnostics)

int regionByName(const char *ukName);   // -1 if unknown
int raionByName(const char *ukName);    // current or pre-2024 name, -1 if unknown
int raionByKey(const char *neptunKey);  // -1 if unknown

// Like the official map, an oblast with alerts in all of its districts counts as FULL.
// vadimklimenko.com/map/statuses.json: oblast "enabled" -> FULL, any district enabled -> PARTIAL;
// districts carry alert_level red/yellow (none = red).
void filterKlymenko(JsonDocument &filter);
bool fromKlymenko(JsonDocument &doc, Snapshot &out);
// Streaming variant for the ESP8266 (the whole document needs ~10 KB): the "states" object is read
// one state at a time, each parsed on its own through filterKlymenkoState.
void filterKlymenkoState(JsonDocument &filter);
void klymenkoState(const char *name, JsonObjectConst state, Snapshot &out);
void clearSnapshot(Snapshot &out);

// in: anything with read() returning -1 at the end (NetReader).
template <typename Input>
bool parseKlymenkoStream(Input &in, Snapshot &out) {
  // Skip to the object after "states".
  static const char KEY[] = "\"states\"";
  int matched = 0, ch;
  while (KEY[matched] && (ch = in.read()) >= 0) matched = ch == KEY[matched] ? matched + 1 : (ch == KEY[0] ? 1 : 0);
  if (KEY[matched]) return lastError = "no states", false;
  while ((ch = in.read()) >= 0 && ch != '{') {}
  if (ch < 0) return lastError = "no states", false;
  clearSnapshot(out);
  JsonDocument filter;
  filterKlymenkoState(filter);
  int states = 0;
  for (;;) {
    // Next key, or the end of "states".
    while ((ch = in.read()) >= 0 && ch != '"' && ch != '}') {}
    if (ch == '}') break;
    if (ch < 0) return lastError = "cut short", false;
    char name[96];
    int n = 0;
    while ((ch = in.read()) >= 0 && ch != '"') {
      if (ch == '\\') ch = in.read();
      if (n < (int)sizeof name - 1) name[n++] = (char)ch;
    }
    name[n] = 0;
    while ((ch = in.read()) >= 0 && ch != ':') {}
    if (ch < 0) return lastError = "cut short", false;
    JsonDocument doc;  // one state with its districts: ~1.5 KB
    DeserializationError e = deserializeJson(doc, in, DeserializationOption::Filter(filter));
    if (e) return lastError = e.c_str(), false;
    klymenkoState(name, doc.as<JsonObjectConst>(), out);
    states++;
  }
  lastError = states ? "" : "empty";
  return states > 0;
}

// ubilling.net.ua/aerialalerts/: "alertnow" per oblast -> FULL (it cannot tell partial alerts);
// regions it does not list (e.g. Crimea) stay UNKNOWN.
void filterUbilling(JsonDocument &filter);
bool fromUbilling(JsonDocument &doc, Snapshot &out);
// neptun.in.ua/api/v1/alerts: "oblasts" (whole oblast) -> FULL, "raions" (by key, level red/yellow) -> PARTIAL.
void filterNeptun(JsonDocument &filter);
bool fromNeptun(JsonDocument &doc, Snapshot &out);

// Parse any ArduinoJson input (Stream/NetReader on the device, std::istream on a PC) through the
// source's filter; the device streams responses instead of holding them in RAM (a 23 KB String
// plus the document ran out of memory).
template <typename Input>
bool parseWith(Input &&in, void (*filter)(JsonDocument &), bool (*from)(JsonDocument &, Snapshot &), Snapshot &out) {
  JsonDocument f, doc;
  filter(f);
  DeserializationError e = deserializeJson(doc, in, DeserializationOption::Filter(f));
  lastError = e.c_str();
  return !e && from(doc, out);
}

inline bool parseWith(const char *json, size_t len, void (*filter)(JsonDocument &),
                      bool (*from)(JsonDocument &, Snapshot &), Snapshot &out) {
  JsonDocument f, doc;
  filter(f);
  DeserializationError e = deserializeJson(doc, json, len, DeserializationOption::Filter(f));
  lastError = e.c_str();
  return !e && from(doc, out);
}

inline bool parseKlymenko(const char *json, size_t len, Snapshot &out) { return parseWith(json, len, filterKlymenko, fromKlymenko, out); }
inline bool parseUbilling(const char *json, size_t len, Snapshot &out) { return parseWith(json, len, filterUbilling, fromUbilling, out); }
inline bool parseNeptun(const char *json, size_t len, Snapshot &out) { return parseWith(json, len, filterNeptun, fromNeptun, out); }
// api.alerts.in.ua IoT: JSON string of 27 chars A/P/N.
bool parseAlertsInUa(const char *json, size_t len, Snapshot &out);

}  // namespace alerts
