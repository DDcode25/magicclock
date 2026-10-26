// SmallTV Pro open firmware. Stock-compatible settings, web UI/API and OTA; see ../SPEC.md.

#include <Arduino.h>
#include <esp_ota_ops.h>
#include "alerts.h"
#include "net.h"
#include "store.h"
#include "ui.h"
#include "web.h"

void setup() {
  Serial.begin(115200);
  Serial.println("\nMagic Box FW " FW_VERSION);
  esp_ota_mark_app_valid_cancel_rollback();

  if (!store::begin()) Serial.println("LittleFS: mount failed (not formatting)");
  settings.load();
  ui::begin();
  ui::message("Magic Box FW", "підключення до Wi-Fi…");
  net::begin();
  web::begin();
  alerts::begin();
  ui::applySettings();
}

void loop() {
  web::loop();
  net::loop();
  ui::loop();
  delay(1);
}
