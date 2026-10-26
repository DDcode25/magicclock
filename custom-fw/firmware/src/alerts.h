#pragma once
#include <TFT_eSPI.h>
#include "alerts_parse.h"

// "Air alerts" app: map of Ukraine coloured by air raid alert status.
// Settings in /.sys/alerts.json: {"src":"klymenko|ubilling|alertsinua","token":"","i":30,"home":-1}
namespace alerts {

void begin();   // load settings, start the background fetch task
void reload();  // settings changed (/set)
void pause(bool on);  // stop fetching during OTA: TLS + upload together run short of RAM

String statusJson();  // {"levels":"NPA...","updated":<utc>,"error":"","src":"..."} for /alerts.json

void draw(TFT_eSPI &tft);  // full redraw of the app screen
void tick(TFT_eSPI &tft);  // called every second while the app is shown

}  // namespace alerts
