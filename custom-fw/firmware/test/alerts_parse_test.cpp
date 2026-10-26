// Host test: g++ -std=c++17 -Isrc -I.pio/libdeps/smalltv-pro/ArduinoJson/src test/alerts_parse_test.cpp src/alerts_parse.cpp
// Usage: a.out <klymenko.json> <ubilling.json> <neptun.json>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include "alerts_parse.h"

static std::string slurp(const char *p) {
  std::ifstream f(p);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

static void print(const char *name, const alerts::Snapshot &s) {
  printf("%-10s ", name);
  for (int i = 0; i < alerts::REGIONS; i++) printf("%c", "NPAU"[s.level[i]]);
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
    std::string j = "{\"states\":{\"Запорізька область\":{\"enabled\":false,\"districts\":{";
    for (int i = 0; i < 5; i++) j += std::string(i ? "," : "") + "\"d" + std::to_string(i) + "\":{\"enabled\":true}";
    j += "}},\"Сумська область\":{\"enabled\":false,\"districts\":{\"a\":{\"enabled\":true},\"b\":{\"enabled\":false}}}}}";
    alerts::Snapshot t{};
    alerts::parseKlymenko(j.data(), j.size(), t);
    if (t.level[7] != alerts::FULL || t.level[19] != alerts::PARTIAL) printf("all-raions rule failed\n"), fails++;
    std::string nj2 = "{\"oblasts\":[],\"raions\":[";
    for (int i = 0; i < 5; i++) nj2 += std::string(i ? "," : "") + "{\"key\":\"k" + std::to_string(i) + "\",\"oblast\":\"Запорізька область\"}";
    nj2 += ",{\"key\":\"k0\",\"oblast\":\"Сумська область\"},{\"key\":\"k0\",\"oblast\":\"Сумська область\"}]}";
    alerts::parseNeptun(nj2.data(), nj2.size(), t);
    if (t.level[7] != alerts::FULL || t.level[19] != alerts::PARTIAL) printf("neptun all-raions rule failed\n"), fails++;
  }
  // occupied regions are alerted as a whole by every source
  for (int i : {0, 12, 18})
    if (n.level[i] != alerts::FULL) printf("neptun: region %d not FULL\n", i), fails++;
  printf("Crimea since %ld (expect 1670710920)\n", (long)k.since[0]);
  if (k.since[0] != 1670710920) fails++;
  printf(fails ? "FAILED %d\n" : "OK\n", fails);
  return fails != 0;
}
