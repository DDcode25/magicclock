#pragma once
// Parsers for air raid alert sources. Plain C++ (no Arduino) so they can be tested on a PC.
// Regions are indexed in alerts.in.ua IoT order (same as MAP[] in ukraine_map.h).
#include <stddef.h>
#include <stdint.h>
#include <time.h>

namespace alerts {

static const int REGIONS = 27;
enum Level : uint8_t { NONE = 0, PARTIAL = 1, FULL = 2, UNKNOWN = 3 };  // UNKNOWN: region absent from source

struct Snapshot {
  uint8_t level[REGIONS];
  time_t since[REGIONS];  // UTC start of the alert, 0 if unknown
};

int regionByName(const char *ukName);  // -1 if unknown

// Like the official map, an oblast with alerts in all of its districts counts as FULL.
// vadimklimenko.com/map/statuses.json: oblast "enabled" -> FULL, any district enabled -> PARTIAL.
bool parseKlymenko(const char *json, size_t len, Snapshot &out);
// ubilling.net.ua/aerialalerts/: "alertnow" per oblast -> FULL (it cannot tell partial alerts);
// regions it does not list (e.g. Crimea) stay UNKNOWN.
bool parseUbilling(const char *json, size_t len, Snapshot &out);
// neptun.in.ua/api/v1/alerts: "oblasts" (whole oblast) -> FULL, "raions" -> PARTIAL.
bool parseNeptun(const char *json, size_t len, Snapshot &out);
// api.alerts.in.ua IoT: JSON string of 27 chars A/P/N.
bool parseAlertsInUa(const char *json, size_t len, Snapshot &out);

}  // namespace alerts
