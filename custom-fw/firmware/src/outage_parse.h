#pragma once
// Power outage schedule parser (Ukrainian "ГПВ" queues). Plain C++ (no Arduino), host-testable.
// Source format (yaroslav2901/OE_OUTAGE_DATA, Baskerville42/outage-data-ua):
//   fact.today = unix time of today's local midnight; fact.update = "26.10.2026 19:58";
//   fact.data[<day ts>]["GPV3.2"]["1".."24"] = yes | no | first | second | mfirst | msecond
//   (hour 1 = 00:00-01:00; first/second = no power in that half hour; m* = possible outage).
#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

namespace outage {

static const int SLOTS = 48;  // half hours per day
enum Slot : uint8_t { ON = 0, OFF = 1, MAYBE = 2, UNKNOWN = 3 };

struct Schedule {
  time_t today = 0;             // local midnight, as given by the source
  bool hasToday = false, hasTomorrow = false;
  uint8_t day[2][SLOTS];        // [0] today, [1] tomorrow
  char update[24] = "";         // source's own "last changed" text
};

// group: "3.2" (the "GPV" prefix is added). Returns false on bad JSON or missing data.
// Files reach 300 KB (Kyiv), so the device parses straight from the network stream through a
// filter that keeps only `group`; `in` is anything ArduinoJson reads (Stream, std::istream, ...).
void makeFilter(const char *group, JsonDocument &filter);
extern const char *lastError;  // why the last parse() failed (diagnostics)
bool fromDoc(const JsonDocument &doc, const char *group, Schedule &out);

template <typename Input>
bool parse(Input &&in, const char *group, Schedule &out) {
  JsonDocument filter, doc;
  makeFilter(group, filter);
  DeserializationError e = deserializeJson(doc, in, DeserializationOption::Filter(filter));
  if (e) {
    lastError = e.c_str();
    return false;
  }
  return fromDoc(doc, group, out);
}

inline bool parse(const char *json, size_t len, const char *group, Schedule &out) {
  JsonDocument filter, doc;
  makeFilter(group, filter);
  if (deserializeJson(doc, json, len, DeserializationOption::Filter(filter))) return false;
  return fromDoc(doc, group, out);
}

}  // namespace outage
