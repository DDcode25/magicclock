#pragma once
#include <TFT_eSPI.h>
#include "alerts_parse.h"

// "Air alerts" app: map of Ukraine coloured by air raid alert status.
// Settings in /.sys/alerts.json:
//   {"src":"klymenko|neptun|ubilling|alertsinua","token":"","i":30,"home":-1,"cc":"#ffffff"}
//   cc: colour of the clock under the map
namespace alerts {

void begin();   // load settings, start the background fetch task
void reload();  // settings changed (/set)
void pause(bool on);  // stop fetching during OTA: TLS + upload together run short of RAM

String statusJson();  // {"levels":"NPA...","updated":<utc>,"error":"","src":"..."} for /alerts.json

// True once when an alert starts (or grows from partial to full) in the home region;
// starts the alarm blink. Not triggered by the first data after boot.
bool homeAlertStarted();
void testAlarm(uint8_t level);  // blink as if the home region got `level` (FULL/PARTIAL)

void draw(TFT_eSPI &tft);  // full redraw of the app screen
void tick(TFT_eSPI &tft);  // called every second while the app is shown
void loop(TFT_eSPI &tft);  // called every UI loop while the app is shown (alarm blink)

}  // namespace alerts
