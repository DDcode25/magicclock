#pragma once
#include <Arduino.h>

namespace ui {

void begin();  // display, backlight, touch calibration ("Don't touch top")
void loop();   // touch input, night brightness, screen refresh

void applySettings();  // after /set: brightness, enabled apps
void next(int dir);    // /set?page=±1, short touch
void enter();          // /set?enter=-1, long touch
bool open(const String &name);
// Full-screen notice (OTA, reboot). Safe from any task: outside the UI loop it is queued and the
// UI loop draws it and keeps it up until the next app change or applySettings().
void message(const String &title, const String &text);
String touchJson();
String stateJson();  // current app and UI state, for /state.json  // {"raw":..,"base":..,"threshold":..,"taps":..,"double":..,"triple":..,"long":..} for /touch.json

}  // namespace ui
