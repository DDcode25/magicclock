#pragma once
#include <TFT_eSPI.h>

// "Світло" app: planned power outage schedule (ГПВ) for one queue, as a 24-hour spiral dial:
// inner ring 0-12 h, outer ring 12-24 h, from the top clockwise, one sector per half hour.
// Data: community JSON mirrors of the oblenergo sites (see outage_parse.h), over HTTPS.
// Settings in /.sys/outage.json: {"src":"y:Poltavaoblenergo","grp":"1.1","i":10}
//   src: "y:<file>" = yaroslav2901/OE_OUTAGE_DATA, "b:<region>" = Baskerville42/outage-data-ua
//   grp: queue "1.1".."6.2"; i: refresh minutes
namespace outagescreen {

void begin();   // start the background fetch task
void reload();  // settings changed (/set)
void pause(bool on);  // stop fetching during OTA

void draw(TFT_eSPI &tft);    // full redraw
void tick(TFT_eSPI &tft);    // every second: new data, time marker once a minute
void toggleDay(TFT_eSPI &tft);  // long press: today <-> tomorrow (when published)

String statusJson();  // for /outage.json

}  // namespace outagescreen
