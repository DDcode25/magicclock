#pragma once
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>

// BearSSL client for public, read-only data (no certificate check: no CA store on the device).
// RAM is the limit on the ESP8266: a 16 KB receive buffer is needed unless the server agrees to a
// smaller TLS fragment (MFLN), so the buffers are sized per host before connecting.
class TlsClient : public BearSSL::WiFiClientSecure {
 public:
  TlsClient() { setInsecure(); }

  // Call before http.begin(): 4 KB receive buffer when the host supports MFLN, else 16 KB. The answer
  // is remembered per host (the probe is an extra handshake).
  void sizeFor(const String &url) {
    struct Known { String host; bool mfln; };
    static Known known[4];
    static int used = 0;
    int s = url.indexOf("://"), e = url.indexOf('/', s + 3);
    String host = url.substring(s + 3, e < 0 ? url.length() : e);
    int i = 0;
    while (i < used && known[i].host != host) i++;
    if (i == used) {
      bool mfln = probeMaxFragmentLength(host, 443, 4096);
      i = used < 4 ? used++ : 3;
      known[i] = {host, mfln};
    }
    if (known[i].mfln) setBufferSizes(4096, 512);
    else setBufferSizes(16384, 512);
  }

  // "помилка HTTP <code>"; for a failed connection (code < 0) also the TLS error and the largest free
  // heap block.
  String httpError(int code) {
    String e = "помилка HTTP " + String(code);
    if (code < 0) e += " tls " + String(getLastSSLError()) + " mem " + String(ESP.getMaxFreeBlockSize() / 1024) + "K";
    return e;
  }
};
