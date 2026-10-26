// SmallTV Pro open firmware: stage 1 (core). Stock-compatible settings, web UI/API and OTA;
// apps are placeholders for now.

#include <Arduino.h>
#include <esp_ota_ops.h>
#include "net.h"
#include "store.h"
#include "ui.h"
#include "web.h"

void setup() {
  Serial.begin(115200);
  Serial.println("\nSmallTV Pro OpenFW " FW_VERSION);
  esp_ota_mark_app_valid_cancel_rollback();

  if (!store::begin()) Serial.println("LittleFS: mount failed (not formatting)");
  settings.load();
  ui::begin();
  ui::message("SmallTV Pro", "connecting Wi-Fi...");
  net::begin();
  web::begin();
  ui::applySettings();
}

void loop() {
  web::loop();
  net::loop();
  ui::loop();
  delay(1);
}
