#include "alerts_parse.h"
#define UKRAINE_MAP_DEFINE  // map data lives in this translation unit
#include "ukraine_map.h"
#include <ArduinoJson.h>
#include <string.h>

namespace alerts {

const char *lastError = "";

// Ukrainian names used by Klymenko and ubilling, in alerts.in.ua IoT order.
static const char *const NAMES[REGIONS] = {
    "АР Крим",
    "Волинська область",
    "Вінницька область",
    "Дніпропетровська область",
    "Донецька область",
    "Житомирська область",
    "Закарпатська область",
    "Запорізька область",
    "Івано-Франківська область",
    "м. Київ",
    "Київська область",
    "Кіровоградська область",
    "Луганська область",
    "Львівська область",
    "Миколаївська область",
    "Одеська область",
    "Полтавська область",
    "Рівненська область",
    "Севастополь",
    "Сумська область",
    "Тернопільська область",
    "Харківська область",
    "Херсонська область",
    "Хмельницька область",
    "Черкаська область",
    "Чернівецька область",
    "Чернігівська область",
};

static int raionCount(int region) {
  static int8_t count[REGIONS] = {-1};
  if (count[0] < 0) {
    memset(count, 0, sizeof count);
    for (int i = 0; i < MAP_RAIONS; i++) count[RAION[i].oblast]++;
  }
  return count[region];
}

static_assert(MAP_RAIONS == RAIONS, "raion count mismatch");

int raionByName(const char *name) {
  if (!name) return -1;
  for (int i = 0; i < MAP_RAIONS; i++)
    if (strcmp(name, RAION[i].name) == 0 || (RAION[i].alt[0] && strcmp(name, RAION[i].alt) == 0)) return i;
  return -1;
}

int raionByKey(const char *key) {
  if (!key) return -1;
  for (int i = 0; i < MAP_RAIONS; i++)
    if (strcmp(key, RAION[i].key) == 0) return i;
  return -1;
}

int regionByName(const char *name) {
  if (!name) return -1;
  for (int i = 0; i < REGIONS; i++)
    if (strcmp(name, NAMES[i]) == 0) return i;
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
    if (RAION[i].oblast == region) s.raion[i] = l;
}

static RaionLevel raionLevel(const char *level) { return level && strcmp(level, "yellow") == 0 ? R_YELLOW : R_RED; }

// Region level from its raions: FULL when all of them are alerted, PARTIAL when some are.
static void levelFromRaions(Snapshot &s, int r) {
  if (s.level[r] == FULL) return;
  int alerted = 0;
  for (int i = 0; i < MAP_RAIONS; i++) alerted += RAION[i].oblast == r && s.raion[i] != R_NONE;
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

bool fromKlymenko(JsonDocument &doc, Snapshot &out) {
  JsonObject states = doc["states"];
  if (states.isNull()) return false;
  clear(out);
  for (JsonPair st : states) {
    int r = regionByName(st.key().c_str());
    if (r < 0) continue;
    JsonObject s = st.value();
    if (s["enabled"] | false) {
      out.level[r] = FULL;
      out.since[r] = parseIso(s["enabled_at"]);
      setRaionsOf(out, r, R_RED);
      continue;
    }
    bool unmatched = false;
    for (JsonPair dp : s["districts"].as<JsonObject>()) {
      JsonObject dist = dp.value();
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
  return true;
}

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
