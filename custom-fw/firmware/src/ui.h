#pragma once
#include <Arduino.h>

namespace ui {

void begin();  // display, backlight, touch calibration ("Don't touch top")
void loop();   // touch input, night brightness, screen refresh

void applySettings();  // after /set: brightness, enabled apps
void next(int dir);    // /set?page=±1, short touch
void enter();          // /set?enter=-1, long touch
bool open(const String &name);
void message(const String &title, const String &text);  // full-screen notice (OTA, reboot)
String touchJson();  // {"raw":..,"base":..,"threshold":..,"taps":..,"double":..,"long":..} for /touch.json

}  // namespace ui
