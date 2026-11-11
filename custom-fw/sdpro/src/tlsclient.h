#pragma once
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>

// BearSSL client for public, read-only data (no certificate check: no CA store on the device).
// RAM is the limit on the ESP8266: a 16 KB receive buffer is needed unless the server agrees to a
// smaller TLS fragment (MFLN), so the buffers are sized per host before connecting.
class TlsClient : public BearSSL::WiFiClientSecure {
 public:
  TlsClient() { setInsecure(); }

  // Call before http.begin(). 4 KB receive buffer when the host agrees to MFLN. Without MFLN the server
  // may send records up to 16 KB, but many never do (Cloudflare: ~1.4 KB records, 2.5 KB handshake), so
  // 4 KB is tried there too; when a fetch with it fails, retryBig() switches the host to 16 KB. The
  // answers are remembered per host (the MFLN probe is an extra handshake).
  void sizeFor(const String &url) {
    int s = url.indexOf("://"), e = url.indexOf('/', s + 3);
    String host = url.substring(s + 3, e < 0 ? url.length() : e);
    static Known known[4];
    static int used = 0;
    int i = 0;
    while (i < used && known[i].host != host) i++;
    if (i == used) {
      bool mfln = probeMaxFragmentLength(host, 443, 4096);
      i = used < 4 ? used++ : 3;
      known[i] = {host, mfln, false};
    }
    entry = &known[i];
    trial = !entry->mfln && !entry->big;
    setBufferSizes(entry->big ? 16384 : 4096, 512);
  }

  // After a failed fetch: true when it ran with the trial 4 KB buffer (a record may not have fit). The
  // host then gets 16 KB from now on and the caller repeats the fetch once.
  bool retryBig() {
    if (!trial) return false;
    entry->big = true;
    trial = false;
    return true;
  }

  // "помилка HTTP <code>"; for a failed connection (code < 0) also the TLS error and the largest free
  // heap block.
  String httpError(int code) {
    String e = "помилка HTTP " + String(code);
    if (code < 0) e += " tls " + String(getLastSSLError()) + " mem " + String(ESP.getMaxFreeBlockSize() / 1024) + "K";
    return e;
  }

 private:
  struct Known {
    String host;
    bool mfln, big;
  };
  Known *entry = nullptr;
  bool trial = false;
};
