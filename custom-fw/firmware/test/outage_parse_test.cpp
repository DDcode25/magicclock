// Host test: g++ -std=c++17 -Isrc -I.pio/libdeps/smalltv-pro/ArduinoJson/src test/outage_parse_test.cpp src/outage_parse.cpp
// Usage: a.out <Poltavaoblenergo.json> [kyiv.json  (stream parse, 300 KB)]
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include "outage_parse.h"

int main(int, char **argv) {
  std::ifstream f(argv[1]);
  std::stringstream ss;
  ss << f.rdbuf();
  std::string j = ss.str();
  int fails = 0;
  for (const char *g : {"1.1", "3.2", "6.2"}) {
    outage::Schedule s;
    if (!outage::parse(j.data(), j.size(), g, s)) { printf("FAIL parse %s\n", g); fails++; continue; }
    printf("%s today=%ld tomorrow=%d update=%s\n  ", g, (long)s.today, s.hasTomorrow, s.update);
    for (int i = 0; i < outage::SLOTS; i++) printf("%c", "+-?."[s.day[0][i]]);
    printf("\n");
    for (int i = 0; i < outage::SLOTS; i++) if (s.day[0][i] == outage::UNKNOWN) { printf("unknown slot %d\n", i); fails++; break; }
  }
  // synthetic: every value kind
  const char *syn = "{\"fact\":{\"today\":1000,\"update\":\"x\",\"data\":{\"1000\":{\"GPV2.1\":{\"1\":\"yes\",\"2\":\"no\",\"3\":\"first\",\"4\":\"second\",\"5\":\"mfirst\",\"6\":\"msecond\"}},\"87400\":{\"GPV2.1\":{\"1\":\"no\"}}}}}";
  outage::Schedule s;
  outage::parse(syn, strlen(syn), "2.1", s);
  const uint8_t want[12] = {0, 0, 1, 1, 1, 0, 0, 1, 2, 0, 0, 2};
  if (memcmp(s.day[0], want, 12) || !s.hasTomorrow || s.day[1][0] != outage::OFF) printf("synthetic mismatch\n"), fails++;
  if (argv[2]) {  // stream parse of a large file, as on the device
    std::ifstream k(argv[2]);
    outage::Schedule ks;
    if (!outage::parse(k, "1.1", ks)) printf("FAIL kyiv stream\n"), fails++;
    else {
      printf("kyiv 1.1 tomorrow=%d  ", ks.hasTomorrow);
      for (int i = 0; i < outage::SLOTS; i++) printf("%c", "+-?."[ks.day[0][i]]);
      printf("\n");
    }
  }
  printf(fails ? "FAILED %d\n" : "OK\n", fails);
  return fails != 0;
}
