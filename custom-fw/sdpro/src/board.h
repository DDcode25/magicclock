#pragma once

// JUZIPi SD PRO (ESP8266). Display pins live in platformio.ini (TFT_eSPI build flags).
static const int PIN_BACKLIGHT = 5;  // active low, PWM

#define MODEL_NAME "UASmartBox SD"  // a macro: pasted into flash strings
static const char *const HOSTNAME = "uasmartbox-sd";
static const char *const AP_SSID = "UASmartBox";
