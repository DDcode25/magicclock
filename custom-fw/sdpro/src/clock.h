#pragma once
#include <TFT_eSPI.h>

// "Clock" app: weekday, large HH:MM, seconds and date, in Ukrainian.
// Stock settings: /.sys/timecolor.json {"hc","mc","sc"} (#RRGGBB for hours, minutes, seconds),
// /.sys/hour12.json {"h":"1"} for 12-hour time, /.sys/day.json {"day":"1".."5"} date format:
// 1 DD/MM/YYYY, 2 YYYY/MM/DD, 3 MM/DD/YYYY, 4 MM/DD, 5 DD/MM (shown with dots).
// Own: /.sys/clock.json {"sm":1} shows the countdown to the next power change ("Світло" app's
// schedule) instead of the seconds.
namespace clockapp {

void draw(TFT_eSPI &tft);  // full redraw (re-reads settings)
void tick(TFT_eSPI &tft);  // every second: seconds, and the rest when it changes

}  // namespace clockapp
