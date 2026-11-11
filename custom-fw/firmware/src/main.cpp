// UA Smart Box firmware for the GeekMagic SmallTV Pro. Stock-compatible settings, web UI/API and OTA; see ../SPEC.md.

#include "fw_version.h"
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
  ui::progress(5);
  net::begin([](int pct) { ui::progress(5 + pct * 70 / 100); });
  ui::progress(75);
  web::begin();
  ui::progress(80);
  alerts::begin();
  ui::progress(85);
  weather::begin();
  ui::progress(90);
  bitcoin::begin();
  ui::progress(95);
  outagescreen::begin();
  ui::progress(100);
  ui::applySettings();
}

void loop() {
  net::loop();
  ui::loop();
  delay(1);
}
