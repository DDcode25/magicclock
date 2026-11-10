#include "alerts_parse.h"
#define UKRAINE_MAP_DEFINE  // map data lives in this translation unit
#include "ukraine_map.h"
#include <ArduinoJson.h>
#include <string.h>

namespace alerts {

const char *lastError = "";

// Ukrainian names used by Klymenko and ubilling, in alerts.in.ua IoT order.
// Flash: RAM is short on the ESP8266.
static const char NAME_0[] PROGMEM = "АР Крим";
static const char NAME_1[] PROGMEM = "Волинська область";
static const char NAME_2[] PROGMEM = "Вінницька область";
static const char NAME_3[] PROGMEM = "Дніпропетровська область";
static const char NAME_4[] PROGMEM = "Донецька область";
static const char NAME_5[] PROGMEM = "Житомирська область";
static const char NAME_6[] PROGMEM = "Закарпатська область";
static const char NAME_7[] PROGMEM = "Запорізька область";
static const char NAME_8[] PROGMEM = "Івано-Франківська область";
static const char NAME_9[] PROGMEM = "м. Київ";
static const char NAME_10[] PROGMEM = "Київська область";
static const char NAME_11[] PROGMEM = "Кіровоградська область";
static const char NAME_12[] PROGMEM = "Луганська область";
static const char NAME_13[] PROGMEM = "Львівська область";
static const char NAME_14[] PROGMEM = "Миколаївська область";
static const char NAME_15[] PROGMEM = "Одеська область";
static const char NAME_16[] PROGMEM = "Полтавська область";
static const char NAME_17[] PROGMEM = "Рівненська область";
static const char NAME_18[] PROGMEM = "Севастополь";
static const char NAME_19[] PROGMEM = "Сумська область";
static const char NAME_20[] PROGMEM = "Тернопільська область";
static const char NAME_21[] PROGMEM = "Харківська область";
static const char NAME_22[] PROGMEM = "Херсонська область";
static const char NAME_23[] PROGMEM = "Хмельницька область";
static const char NAME_24[] PROGMEM = "Черкаська область";
static const char NAME_25[] PROGMEM = "Чернівецька область";
static const char NAME_26[] PROGMEM = "Чернігівська область";
static const char *const NAMES[REGIONS] PROGMEM = {NAME_0, NAME_1, NAME_2, NAME_3, NAME_4, NAME_5, NAME_6, NAME_7, NAME_8, NAME_9, NAME_10, NAME_11, NAME_12, NAME_13, NAME_14, NAME_15, NAME_16, NAME_17, NAME_18, NAME_19, NAME_20, NAME_21, NAME_22, NAME_23, NAME_24, NAME_25, NAME_26};

static int raionCount(int region) {
  static int8_t count[REGIONS] = {-1};
  if (count[0] < 0) {
    memset(count, 0, sizeof count);
    for (int i = 0; i < MAP_RAIONS; i++) count[mapRaion(i).oblast]++;
  }
  return count[region];
}

static_assert(MAP_RAIONS == RAIONS, "raion count mismatch");

int raionByName(const char *name) {
  if (!name) return -1;
  for (int i = 0; i < MAP_RAIONS; i++) {
    MapRaion r = mapRaion(i);
    if (strcmp_P(name, r.name) == 0 || (pgm_read_byte(r.alt) && strcmp_P(name, r.alt) == 0)) return i;
  }
  return -1;
}

int raionByKey(const char *key) {
  if (!key) return -1;
  for (int i = 0; i < MAP_RAIONS; i++)
    if (strcmp_P(key, mapRaion(i).key) == 0) return i;
  return -1;
}

int regionByName(const char *name) {
  if (!name) return -1;
  for (int i = 0; i < REGIONS; i++)
    if (strcmp_P(name, (const char *)pgm_read_ptr(&NAMES[i])) == 0) return i;
  if (strcmp(name, "Автономна Республіка Крим") == 0) return 0;
  if (strcmp(name, "м. Севастополь") == 0) return 18;
  if (strcmp(name, "Київ") == 0 || strcmp(name, "м.Київ") == 0 || strcmp(name, "місто Київ") == 0) return 9;
  return -1;
}

static void clear(Snapshot &s, Level fill = NONE, RaionLevel raionFill = R_NONE) {
  memset(s.level, fill, sizeof s.level);
  memset(s.since, 0, sizeof s.since);
  memset(s.raion, raionFill, sizeof s.raion);
}

static void setRaionsOf(Snapshot &s, int region, RaionLevel l) {
  for (int i = 0; i < MAP_RAIONS; i++)
    if (mapRaion(i).oblast == region) s.raion[i] = l;
}

static RaionLevel raionLevel(const char *level) { return level && strcmp(level, "yellow") == 0 ? R_YELLOW : R_RED; }

// Region level from its raions: FULL when all of them are alerted, PARTIAL when some are.
static void levelFromRaions(Snapshot &s, int r) {
  if (s.level[r] == FULL) return;
  int alerted = 0;
  for (int i = 0; i < MAP_RAIONS; i++) alerted += mapRaion(i).oblast == r && s.raion[i] != R_NONE;
  if (alerted) s.level[r] = alerted >= raionCount(r) ? FULL : PARTIAL;
}

// days since 1970-01-01 for a civil date (Howard Hinnant's algorithm)
static long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

// "2022-12-10T22:22:00+00:00" -> UTC epoch, 0 on failure
static time_t parseIso(const char *s) {
  int y, mo, d, h, mi, sec, oh = 0, om = 0;
  char sign = '+';
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
  const char *tz = s + 19;
  while (*tz == '.' || (*tz >= '0' && *tz <= '9')) tz++;
  if ((*tz == '+' || *tz == '-') && sscanf(tz + 1, "%d:%d", &oh, &om) == 2) sign = *tz;
  long t = daysFromCivil(y, mo, d) * 86400L + h * 3600L + mi * 60L + sec;
  long off = (oh * 60L + om) * 60L;
  return (time_t)(sign == '+' ? t - off : t + off);
}

void filterKlymenko(JsonDocument &filter) {
  filter["states"]["*"]["enabled"] = true;
  filter["states"]["*"]["enabled_at"] = true;
  filter["states"]["*"]["districts"]["*"]["enabled"] = true;
  filter["states"]["*"]["districts"]["*"]["enabled_at"] = true;
  filter["states"]["*"]["districts"]["*"]["alert_level"] = true;
}

void filterKlymenkoState(JsonDocument &filter) {
  filter["enabled"] = true;
  filter["enabled_at"] = true;
  filter["districts"]["*"]["enabled"] = true;
  filter["districts"]["*"]["enabled_at"] = true;
  filter["districts"]["*"]["alert_level"] = true;
}

void klymenkoState(const char *name, JsonObjectConst s, Snapshot &out) {
  int r = regionByName(name);
  if (r < 0) return;
  if (s["enabled"] | false) {
    out.level[r] = FULL;
    out.since[r] = parseIso(s["enabled_at"]);
    setRaionsOf(out, r, R_RED);
    return;
  }
  bool unmatched = false;
  for (JsonPairConst dp : s["districts"].as<JsonObjectConst>()) {
    JsonObjectConst dist = dp.value();
    if (!(dist["enabled"] | false)) continue;
    int d = raionByName(dp.key().c_str());
    if (d >= 0) out.raion[d] = raionLevel(dist["alert_level"]);
    else unmatched = true;
    time_t t = parseIso(dist["enabled_at"]);
    if (t && (!out.since[r] || t < out.since[r])) out.since[r] = t;
  }
  levelFromRaions(out, r);
  if (unmatched && out.level[r] == NONE) out.level[r] = PARTIAL;
}

bool fromKlymenko(JsonDocument &doc, Snapshot &out) {
  JsonObject states = doc["states"];
  if (states.isNull()) return false;
  clear(out);
  for (JsonPair st : states) klymenkoState(st.key().c_str(), st.value(), out);
  return true;
}

void clearSnapshot(Snapshot &out) { clear(out); }

void filterUbilling(JsonDocument &filter) {
  filter["states"]["*"]["alertnow"] = true;
}

bool fromUbilling(JsonDocument &doc, Snapshot &out) {
  JsonObject states = doc["states"];
  if (states.isNull()) return false;
  clear(out, UNKNOWN, R_INHERIT);
  for (JsonPair st : states) {
    int r = regionByName(st.key().c_str());
    if (r >= 0) out.level[r] = (st.value()["alertnow"] | false) ? FULL : NONE;
  }
  return true;
}

void filterNeptun(JsonDocument &filter) {
  filter["oblasts"][0]["name"] = true;
  filter["oblasts"][0]["since"] = true;
  filter["raions"][0]["oblast"] = true;
  filter["raions"][0]["key"] = true;
  filter["raions"][0]["since"] = true;
  filter["raions"][0]["level"] = true;
}

bool fromNeptun(JsonDocument &doc, Snapshot &out) {
  JsonArray oblasts = doc["oblasts"], raions = doc["raions"];
  if (oblasts.isNull() || raions.isNull()) return false;
  clear(out);
  for (JsonObject o : oblasts) {
    int r = regionByName(o["name"]);
    if (r < 0) continue;
    out.level[r] = FULL;
    out.since[r] = parseIso(o["since"]);
    setRaionsOf(out, r, R_RED);
  }
  // A raion may be listed more than once (one entry per threat): keep the highest level.
  bool unmatched[REGIONS] = {};
  for (JsonObject d : raions) {
    int r = regionByName(d["oblast"]);
    if (r < 0 || out.level[r] == FULL) continue;
    int k = raionByKey(d["key"]);
    if (k >= 0) {
      RaionLevel l = raionLevel(d["level"]);
      if (out.raion[k] == R_NONE || l > out.raion[k]) out.raion[k] = l;
    } else {
      unmatched[r] = true;
    }
    time_t t = parseIso(d["since"]);
    if (t && (!out.since[r] || t < out.since[r])) out.since[r] = t;
  }
  for (int r = 0; r < REGIONS; r++) {
    levelFromRaions(out, r);
    if (unmatched[r] && out.level[r] == NONE) out.level[r] = PARTIAL;
  }
  return true;
}

bool parseAlertsInUa(const char *json, size_t len, Snapshot &out) {
  const char *q = (const char *)memchr(json, '"', len);
  if (!q || (size_t)(q - json) + 1 + REGIONS > len) return false;
  clear(out, NONE, R_INHERIT);
  for (int i = 0; i < REGIONS; i++) {
    char c = q[1 + i];
    if (c != 'A' && c != 'P' && c != 'N') return false;
    out.level[i] = c == 'A' ? FULL : c == 'P' ? PARTIAL : NONE;
  }
  return q[1 + REGIONS] == '"';
}

}  // namespace alerts
