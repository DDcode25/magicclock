#pragma once
#include <Arduino.h>

namespace net {

// Connects as STA with /.sys/config.json {"a":ssid,"p":pass}. If that fails (or the link
// stays down), opens the AP "SmallTV-PRO" at 192.168.4.1 with a captive DNS so the device
// can always be reached for setup and OTA.
void begin();
void loop();

bool connected();
bool apActive();
String ip();     // STA IP, or AP IP when only the AP is up
String ssid();

void applyTime();      // (re)configure NTP and timezone from settings
bool timeValid();

String scanJson();     // {"aps":[{"ss":..,"r":quality%,"e":0|1}]} for /wifi.json
bool saveWifi(const String &ssid, const String &pass);

}  // namespace net
