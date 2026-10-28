#pragma once

// GeekMagic SmallTV Pro. Display pins live in platformio.ini (TFT_eSPI build flags).
static const int PIN_BACKLIGHT = 25;
static const int BL_CHANNEL = 0;  // LEDC channel, 5 kHz, 8-bit (same as stock)
static const int PIN_TOUCH = 32;  // capacitive pad on top of the case (T9)

static const char *MODEL_NAME = "UASmartBox";
static const char *HOSTNAME = "uasmartbox";
static const char *AP_SSID = "UASmartBox";
