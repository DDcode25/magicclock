// UA Smart Box firmware for the JUZIPi SD PRO (ESP8266). Copy of ../firmware reduced to the weather,
// clock, alerts and outage apps. Single-threaded: background fetches run one at a time from loop().

#include "fw_version.h"
#include <Arduino.h>
#include "alerts.h"
#include "diag.h"
#include "net.h"
#include "outage.h"
#include "store.h"
#include "ui.h"
#include "weather.h"
#include "web.h"

void setup() {
  Serial.begin(115200);
  Serial.println("\nUA Smart Box " FW_VERSION);
  if (!store::begin()) Serial.println("LittleFS: mount failed");
  diag::begin();
  if (diag::safeMode()) {
    Serial.println("Safe mode: Wi-Fi and web (OTA) only");
    settings.load();
    net::begin();
    web::begin();
    return;
  }
  settings.load();
  ui::begin();
  ui::message("UA Smart Box", "підключення до Wi-Fi…");
  net::begin();
  web::begin();
  alerts::begin();
  weather::begin();
  outagescreen::begin();
  ui::applySettings();
  ui::showIp();
}

// One background fetch per pass at most, so the screen and the web server get time in between.
static void pollFetches() {
  static int next = 0;
  for (int i = 0; i < 3; i++) {
    int k = (next + i) % 3;
    bool ran = k == 0 ? alerts::poll() : k == 1 ? weather::poll() : outagescreen::poll();
    if (ran) {
      next = (k + 1) % 3;
      return;
    }
  }
}

void loop() {
  diag::loop();
  net::loop();
  web::loop();
  if (diag::safeMode()) return delay(1);
  ui::loop();
  pollFetches();
  delay(1);
}
