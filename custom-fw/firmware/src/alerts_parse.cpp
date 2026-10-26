#include "alerts_parse.h"
#include <ArduinoJson.h>
#include <string.h>

namespace alerts {

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

// Number of raions per region since the 2020 reform (0: city or not split), same order as NAMES.
static const uint8_t RAIONS[REGIONS] = {0, 4, 6, 7, 8, 4, 6, 5, 6, 0, 7, 4, 8, 7,
                                        4, 7, 4, 4, 0, 5, 3, 7, 5, 3, 4, 3, 5};

int regionByName(const char *name) {
  if (!name) return -1;
  for (int i = 0; i < REGIONS; i++)
    if (strcmp(name, NAMES[i]) == 0) return i;
  if (strcmp(name, "Автономна Республіка Крим") == 0) return 0;
  if (strcmp(name, "м. Севастополь") == 0) return 18;
  if (strcmp(name, "Київ") == 0 || strcmp(name, "м.Київ") == 0 || strcmp(name, "місто Київ") == 0) return 9;
  return -1;
}

static void clear(Snapshot &s, Level fill = NONE) {
  memset(s.level, fill, sizeof s.level);
  memset(s.since, 0, sizeof s.since);
}

// Partial alert in `alerted` raions of region r: FULL when that is all of them.
static void markRaions(Snapshot &s, int r, int alerted) {
  if (alerted <= 0 || s.level[r] == FULL) return;
  s.level[r] = RAIONS[r] && alerted >= RAIONS[r] ? FULL : PARTIAL;
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

bool parseKlymenko(const char *json, size_t len, Snapshot &out) {
  JsonDocument filter;
  filter["states"]["*"]["enabled"] = true;
  filter["states"]["*"]["enabled_at"] = true;
  filter["states"]["*"]["districts"]["*"]["enabled"] = true;
  filter["states"]["*"]["districts"]["*"]["enabled_at"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, json, len, DeserializationOption::Filter(filter))) return false;
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
      continue;
    }
    int alerted = 0;
    for (JsonPair dp : s["districts"].as<JsonObject>()) {
      JsonObject dist = dp.value();
      if (!(dist["enabled"] | false)) continue;
      alerted++;
      time_t t = parseIso(dist["enabled_at"]);
      if (t && (!out.since[r] || t < out.since[r])) out.since[r] = t;
    }
    markRaions(out, r, alerted);
  }
  return true;
}

bool parseUbilling(const char *json, size_t len, Snapshot &out) {
  JsonDocument filter;
  filter["states"]["*"]["alertnow"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, json, len, DeserializationOption::Filter(filter))) return false;
  JsonObject states = doc["states"];
  if (states.isNull()) return false;
  clear(out, UNKNOWN);
  for (JsonPair st : states) {
    int r = regionByName(st.key().c_str());
    if (r >= 0) out.level[r] = (st.value()["alertnow"] | false) ? FULL : NONE;
  }
  return true;
}

bool parseNeptun(const char *json, size_t len, Snapshot &out) {
  JsonDocument filter;
  filter["oblasts"][0]["name"] = true;
  filter["oblasts"][0]["since"] = true;
  filter["raions"][0]["oblast"] = true;
  filter["raions"][0]["key"] = true;
  filter["raions"][0]["since"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, json, len, DeserializationOption::Filter(filter))) return false;
  JsonArray oblasts = doc["oblasts"], raions = doc["raions"];
  if (oblasts.isNull() || raions.isNull()) return false;
  clear(out);
  for (JsonObject o : oblasts) {
    int r = regionByName(o["name"]);
    if (r < 0) continue;
    out.level[r] = FULL;
    out.since[r] = parseIso(o["since"]);
  }
  // A raion may be listed more than once (one entry per threat); count distinct keys.
  const char *seen[REGIONS][8] = {};
  int alerted[REGIONS] = {};
  for (JsonObject d : raions) {
    int r = regionByName(d["oblast"]);
    if (r < 0 || out.level[r] == FULL) continue;
    const char *key = d["key"] | "";
    bool dup = false;
    for (int i = 0; i < alerted[r] && i < 8; i++) dup |= strcmp(seen[r][i], key) == 0;
    if (!dup) {
      if (alerted[r] < 8) seen[r][alerted[r]] = key;
      alerted[r]++;
    }
    time_t t = parseIso(d["since"]);
    if (t && (!out.since[r] || t < out.since[r])) out.since[r] = t;
  }
  for (int r = 0; r < REGIONS; r++) markRaions(out, r, alerted[r]);
  return true;
}

bool parseAlertsInUa(const char *json, size_t len, Snapshot &out) {
  const char *q = (const char *)memchr(json, '"', len);
  if (!q || (size_t)(q - json) + 1 + REGIONS > len) return false;
  clear(out);
  for (int i = 0; i < REGIONS; i++) {
    char c = q[1 + i];
    if (c != 'A' && c != 'P' && c != 'N') return false;
    out.level[i] = c == 'A' ? FULL : c == 'P' ? PARTIAL : NONE;
  }
  return q[1 + REGIONS] == '"';
}

}  // namespace alerts
