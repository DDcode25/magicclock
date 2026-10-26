// Host test: g++ -std=c++17 -Isrc -I.pio/libdeps/smalltv-pro/ArduinoJson/src test/alerts_parse_test.cpp src/alerts_parse.cpp
// Usage: a.out <klymenko.json> <ubilling.json> <neptun.json>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include "alerts_parse.h"
#include "ukraine_map.h"

static std::string slurp(const char *p) {
  std::ifstream f(p);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

static void print(const char *name, const alerts::Snapshot &s) {
  printf("%-10s ", name);
  for (int i = 0; i < alerts::REGIONS; i++) printf("%c", "NPAU"[s.level[i]]);
  printf("  raions ");
  for (int i = 0; i < alerts::RAIONS; i++) printf("%c", "NYRI"[s.raion[i]]);
  printf("\n");
}


int main(int argc, char **argv) {
  int fails = 0;
  alerts::Snapshot k{}, u{}, a{}, n{};
  std::string kj = slurp(argv[1]), uj = slurp(argv[2]), nj = slurp(argv[3]);
  if (!alerts::parseKlymenko(kj.data(), kj.size(), k)) printf("FAIL klymenko\n"), fails++;
  if (!alerts::parseUbilling(uj.data(), uj.size(), u)) printf("FAIL ubilling\n"), fails++;
  if (!alerts::parseNeptun(nj.data(), nj.size(), n)) printf("FAIL neptun\n"), fails++;
  const char *iot = "\"APNNNNNNNNNNANNNNNNNNNNNNNP\"";
  if (!alerts::parseAlertsInUa(iot, strlen(iot), a)) printf("FAIL alerts.in.ua\n"), fails++;
  print("klymenko", k);
  print("ubilling", u);
  print("neptun", n);
  print("alertsinua", a);
  // ubilling marks an oblast if any district has an alert, so it must match klymenko's FULL|PARTIAL
  for (int i = 0; i < alerts::REGIONS; i++)
    if (u.level[i] != alerts::UNKNOWN && (k.level[i] != alerts::NONE) != (u.level[i] != alerts::NONE)) printf("mismatch region %d\n", i), fails++;
  if (u.level[0] != alerts::UNKNOWN) printf("ubilling: Crimea should be UNKNOWN\n"), fails++;
  // all raions alerted -> whole oblast, e.g. {"Донецька область":{"enabled":false,"districts":{8 x enabled}}}
  {
    // all 5 Zaporizhzhia raions (real names) + 1 of Sumy
    std::string j = "{\"states\":{\"Запорізька область\":{\"enabled\":false,\"districts\":{", nj2 = "{\"oblasts\":[],\"raions\":[";
    bool first = true;
    for (int i = 0; i < MAP_RAIONS; i++)
      if (RAION[i].oblast == 7) {
        j += std::string(first ? "" : ",") + "\"" + RAION[i].name + "\":{\"enabled\":true,\"alert_level\":\"yellow\"}";
        nj2 += std::string(first ? "" : ",") + "{\"key\":\"" + RAION[i].key + "\",\"oblast\":\"Запорізька область\",\"level\":\"yellow\"}";
        first = false;
      }
    int sumy = -1;
    for (int i = 0; i < MAP_RAIONS && sumy < 0; i++)
      if (RAION[i].oblast == 19) sumy = i;
    j += std::string("}},\"Сумська область\":{\"enabled\":false,\"districts\":{\"") + RAION[sumy].name + "\":{\"enabled\":true}}}}}";
    nj2 += std::string(",{\"key\":\"") + RAION[sumy].key + "\",\"oblast\":\"Сумська область\",\"level\":\"yellow\"}" +
           ",{\"key\":\"" + RAION[sumy].key + "\",\"oblast\":\"Сумська область\",\"level\":\"red\"}]}";
    alerts::Snapshot t{};
    alerts::parseKlymenko(j.data(), j.size(), t);
    if (t.level[7] != alerts::FULL || t.level[19] != alerts::PARTIAL) printf("all-raions rule failed\n"), fails++;
    if (t.raion[sumy] != alerts::R_RED || t.raion[sumy + 1] != alerts::R_NONE) printf("klymenko raion levels wrong\n"), fails++;
    alerts::parseNeptun(nj2.data(), nj2.size(), t);
    if (t.level[7] != alerts::FULL || t.level[19] != alerts::PARTIAL) printf("neptun all-raions rule failed\n"), fails++;
    if (t.raion[sumy] != alerts::R_RED) printf("neptun: duplicate raion should keep the highest level\n"), fails++;
  }
  // occupied regions are alerted as a whole by every source
  for (int i : {0, 12, 18})
    if (n.level[i] != alerts::FULL) printf("neptun: region %d not FULL\n", i), fails++;
  for (size_t p = 0; (p = nj.find("\"key\":\"", p)) != std::string::npos; p += 7) {
    size_t e = nj.find('"', p + 7);
    std::string key = nj.substr(p + 7, e - p - 7);
    if (alerts::raionByKey(key.c_str()) < 0 && nj.rfind("\"raions\"", p) != std::string::npos &&
        nj.rfind("\"raions\"", p) > nj.rfind("\"oblasts\"", p))
      printf("neptun: unknown raion key %s\n", key.c_str()), fails++;
  }
  // raion-level data must agree with the region level
  for (const alerts::Snapshot *x : {&k, &n})
    for (int i = 0; i < alerts::RAIONS; i++)
      if (x->raion[i] != alerts::R_NONE && x->level[RAION[i].oblast] == alerts::NONE) printf("raion %d alerted but region clear\n", i), fails++;
  for (int i = 0; i < alerts::RAIONS; i++)
    if (u.raion[i] != alerts::R_INHERIT || a.raion[i] != alerts::R_INHERIT) printf("oblast-only source has raion data\n"), fails++, i = alerts::RAIONS;
  printf("Crimea since %ld (expect 1670710920)\n", (long)k.since[0]);
  if (k.since[0] != 1670710920) fails++;
  printf(fails ? "FAILED %d\n" : "OK\n", fails);
  return fails != 0;
}
