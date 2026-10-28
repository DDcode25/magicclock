#pragma once
#include <WiFiClientSecure.h>

// WiFiClientSecure with a fix for Arduino-ESP32 2.0.x: stop() ends with a memset of the SSL context,
// which leaves socket = 0 instead of -1, so the next stop() (HTTPClient's destructor, then the
// client's own) calls close(0). File descriptor 0 is usually a LittleFS file another task has open
// (web server, GIF player): it gets closed under that task, and LittleFS asserts (panic).
// Keeping socket at -1 after every stop makes the extra calls harmless.
class TlsClient : public WiFiClientSecure {
 public:
  // "помилка HTTP <code>"; for a failed connection (code < 0) also the TLS error and the largest free
  // heap block (a TLS handshake needs two ~17 KB buffers).
  String httpError(int code) {
    String e = "помилка HTTP " + String(code);
    if (code < 0) {
      char buf[8];
      e += " tls " + String(lastError(buf, sizeof buf)) + " mem " + String(ESP.getMaxAllocHeap() / 1024) + "K";
    }
    return e;
  }
  TlsClient() { setInsecure(); }  // public, read-only data; no CA bundle on the device
  ~TlsClient() { stop(); }        // leaves -1 for the base destructor's stop()
  void stop() override {
    WiFiClientSecure::stop();
    sslclient->socket = -1;
  }
};
