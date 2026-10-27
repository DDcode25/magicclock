#pragma once
#include <Arduino.h>

namespace net {

// Remembers the last 5 networks (/.sys/wifis.json, newest first; the stock /.sys/config.json
// {"a":ssid,"p":pass} holds the last saved one) and connects to the strongest of them in range.
// If none is reachable (or the link stays down), opens the AP "SmallTV-PRO" at 192.168.4.1 with a
// captive DNS so the device can always be reached for setup and OTA; while offline it looks for a
// known network every minute (not while a client is on the AP).
void begin();
void loop();

bool connected();
bool apActive();
String ip();     // STA IP, or AP IP when only the AP is up
String ssid();

void applyTime();      // (re)configure NTP and timezone from settings

// Background fetches (alerts over TLS, weather) take this so they never run at the same time:
// two at once run short of RAM next to the map sprite and the GIF decoder.
void fetchLock();
void fetchUnlock();
bool timeValid();

String scanJson();     // {"aps":[{"ss":..,"r":quality%,"e":0|1}]} for /wifi.json
bool saveWifi(const String &ssid, const String &pass);  // also remembers it
String knownJson();    // {"list":[ssid,...],"current":ssid} for /wifiknown.json (no passwords)
bool forgetWifi(const String &ssid);

}  // namespace net
