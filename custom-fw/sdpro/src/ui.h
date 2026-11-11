#pragma once
#include <Arduino.h>

namespace ui {

void begin();  // display, backlight
void loop();   // auto rotation of apps, night brightness, screen refresh

void applySettings();  // after /set: brightness, enabled apps
void next(int dir);    // /set?page=±1
void enter();          // /set?enter=-1: outage today <-> tomorrow
bool open(const String &name);
void showIp();         // boot: network and IP for a few seconds (there is no touch to ask for it)
// Full-screen notice (OTA, reboot), kept up until the next app change or applySettings().
void message(const String &title, const String &text);
void progress(int pct);  // 0..100: a bar under the message
String stateJson();  // current app and UI state, for /state.json

}  // namespace ui
