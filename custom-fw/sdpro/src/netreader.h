#pragma once
#include <WiFiClient.h>

// Blocking reader for ArduinoJson over a (TLS) client: the stream returns "nothing yet" between
// records, which ArduinoJson takes for the end of input (EmptyInput). Waits for data while the
// link is open. Parsing straight from the stream avoids holding whole responses in RAM.
struct NetReader {
  WiFiClient &c;
  uint32_t timeoutMs;
  int read() {
    uint32_t t0 = millis();
    while (!c.available()) {
      if (!c.connected() || millis() - t0 > timeoutMs) return -1;
      delay(2);
    }
    return c.read();
  }
  size_t readBytes(char *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
      int ch = read();
      if (ch < 0) break;
      buf[got++] = (char)ch;
      int more = min((int)(n - got), c.available());
      if (more > 0) got += c.read((uint8_t *)buf + got, more);
    }
    return got;
  }
};
