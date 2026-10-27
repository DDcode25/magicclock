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
