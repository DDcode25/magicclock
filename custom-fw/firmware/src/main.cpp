// UA Smart Box firmware for the GeekMagic SmallTV Pro. Stock-compatible settings, web UI/API and OTA; see ../SPEC.md.

#include <Arduino.h>
#include <esp_ota_ops.h>
#include "alerts.h"
#include "diag.h"
#include "net.h"
#include "outage.h"
#include "store.h"
#include "ui.h"
#include "bitcoin.h"
#include "weather.h"
#include "web.h"

void setup() {
  Serial.begin(115200);
  Serial.println("\nUA Smart Box " FW_VERSION);
  esp_ota_mark_app_valid_cancel_rollback();

  net::reserveTls();  // while the heap is still in one piece
  if (!store::begin()) Serial.println("LittleFS: mount failed (not formatting)");
  diag::begin();
  settings.load();
  ui::begin();
  ui::message("UA Smart Box", "підключення до Wi-Fi…");
  net::begin();
  web::begin();
  alerts::begin();
  weather::begin();
  bitcoin::begin();
  outagescreen::begin();
  ui::applySettings();
}

void loop() {
  net::loop();
  ui::loop();
  delay(1);
}
