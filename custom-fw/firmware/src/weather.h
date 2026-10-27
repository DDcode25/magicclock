#pragma once
#include <TFT_eSPI.h>

// "Weather" app: current weather from Open-Meteo (free, no key), in Ukrainian.
// Stock settings: /.sys/city.json {"cd1":<city name>,"nickname":<display name>,"w_i":<minutes>},
// /.sys/unit.json {"t_u":"°C|°F","w_u":"m/s|km/h|mile/h","p_u":"hPa|kPa|mmHg|inHg"}.
// The city is resolved to coordinates with the Open-Meteo geocoder (Ukrainian name for display).
namespace weather {

void begin();   // start the background fetch task
void reload();  // settings changed (/set)
void pause(bool on);  // stop fetching during OTA

void draw(TFT_eSPI &tft);  // full redraw
void tick(TFT_eSPI &tft);  // every second: redraw when new data arrived

String statusJson();  // for /weather.json

}  // namespace weather
