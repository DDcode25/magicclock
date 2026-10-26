#pragma once
// Parsers for air raid alert sources. Plain C++ (no Arduino) so they can be tested on a PC.
// Regions are indexed in alerts.in.ua IoT order (same as MAP[] in ukraine_map.h),
// raions like RAION[] in ukraine_map.h.
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

int regionByName(const char *ukName);   // -1 if unknown
int raionByName(const char *ukName);    // current or pre-2024 name, -1 if unknown
int raionByKey(const char *neptunKey);  // -1 if unknown

// Like the official map, an oblast with alerts in all of its districts counts as FULL.
// vadimklimenko.com/map/statuses.json: oblast "enabled" -> FULL, any district enabled -> PARTIAL;
// districts carry alert_level red/yellow (none = red).
bool parseKlymenko(const char *json, size_t len, Snapshot &out);
// ubilling.net.ua/aerialalerts/: "alertnow" per oblast -> FULL (it cannot tell partial alerts);
// regions it does not list (e.g. Crimea) stay UNKNOWN.
bool parseUbilling(const char *json, size_t len, Snapshot &out);
// neptun.in.ua/api/v1/alerts: "oblasts" (whole oblast) -> FULL, "raions" (by key, level red/yellow) -> PARTIAL.
bool parseNeptun(const char *json, size_t len, Snapshot &out);
// api.alerts.in.ua IoT: JSON string of 27 chars A/P/N.
bool parseAlertsInUa(const char *json, size_t len, Snapshot &out);

}  // namespace alerts
