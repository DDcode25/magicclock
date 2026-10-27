#pragma once
#include <TFT_eSPI.h>

// "Світло" app: planned power outage schedule (ГПВ) for one queue, as a 24-hour spiral dial:
// inner ring 0-12 h, outer ring 12-24 h, from the top clockwise, one sector per half hour.
// Data over HTTPS: the official Poltavaoblenergo page, or community JSON mirrors of the oblenergo
// sites (see outage_parse.h).
// Settings in /.sys/outage.json: {"src":"poe","grp":"1.1","i":10}
//   src: "poe" = poe.pl.ua/customs/dynamicgpv-info.php (official, Poltava),
//        "y:<file>" = yaroslav2901/OE_OUTAGE_DATA, "b:<region>" = Baskerville42/outage-data-ua
//   grp: queue "1.1".."6.2"; i: refresh minutes
namespace outagescreen {

void begin();   // start the background fetch task
void reload();  // settings changed (/set)
void pause(bool on);  // stop fetching during OTA

void draw(TFT_eSPI &tft);    // full redraw
void tick(TFT_eSPI &tft);    // every second: new data, time marker once a minute
void toggleDay(TFT_eSPI &tft);  // long press: today <-> tomorrow (when published)

String statusJson();  // for /outage.json

// For the clock: when the power state next changes after `now` (OFF = no power; possible-outage
// half hours count as power). powerOn = the state after the change. false without a schedule for
// now or without a change in the published days.
bool nextChange(time_t now, bool &powerOn, time_t &at);

}  // namespace outagescreen
