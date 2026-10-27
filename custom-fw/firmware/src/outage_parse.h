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

// ---- Poltavaoblenergo official page (poe.pl.ua/customs/dynamicgpv-info.php) -------------------
// HTML: per day a block "... <b>27 жовтня 2026 року</b> ..." with a table; rows carry the queue
// ("1 черга") and subqueue ("1"/"2") cells, then 48 half-hour cells class="light_1|2|3"
// (1 = power on, 2 = off, 3 = possible). Parsed as a character stream, nothing buffered.
struct PoeDay {
  int year = 0, month = 0, day = 0;  // date of the block
  bool found = false;                // the requested group had a full row
  uint8_t slots[SLOTS];
};

class PoeParser {
 public:
  explicit PoeParser(const char *group);  // "1.1"
  void feed(char c);
  int days() const { return count; }
  const PoeDay &day(int i) const { return found[i]; }

 private:
  enum Want { NONE, DATE, QUEUE, SUBQUEUE };
  void onTag();
  void onText();
  int wantQueue, wantSub;
  int queue = 0, sub = 0, cells = 0;
  uint8_t row[SLOTS];
  PoeDay found[2];
  int count = 0, curY = 0, curM = 0, curD = 0;
  bool inTag = false, inB = false;
  Want want = NONE;
  char tag[96];
  int tagLen = 0;
  char text[64];
  int textLen = 0;
};

}  // namespace outage
