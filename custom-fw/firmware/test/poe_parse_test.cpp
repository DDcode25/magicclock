// Host test: g++ -std=c++17 -Isrc -I.pio/libdeps/smalltv-pro/ArduinoJson/src test/poe_parse_test.cpp src/outage_parse.cpp
// Usage: a.out <dynamicgpv-info.html>
#include <cstdio>
#include <fstream>
#include "outage_parse.h"

int main(int, char **argv) {
  int fails = 0;
  for (const char *g : {"1.1", "1.2", "3.2", "6.2"}) {
    outage::PoeParser p(g);
    std::ifstream f(argv[1], std::ios::binary);
    char c;
    while (f.get(c)) p.feed(c);
    printf("%s days=%d", g, p.days());
    for (int d = 0; d < p.days(); d++) {
      printf("  %04d-%02d-%02d ", p.day(d).year, p.day(d).month, p.day(d).day);
      for (int i = 0; i < outage::SLOTS; i++) printf("%c", "+-?."[p.day(d).slots[i]]);
    }
    printf("\n");
    if (p.days() < 1) fails++;
  }
  printf(fails ? "FAILED %d\n" : "OK\n", fails);
  return fails != 0;
}
