#pragma once
#include <TFT_eSPI.h>

// Small UI pieces shared by several apps.

// Tiny Wi-Fi mark in the top-right corner (13x10 px, x 220..238, y 3..13): dot and three arcs.
// on=false erases it. Grey while connected (callers blink it once a second), red when the link is
// down (callers keep it on).
void wifiMark(TFT_eSPI &tft, bool on);
// Call once a second: blinks the mark while connected, keeps it on (red) without a link.
void wifiMarkTick(TFT_eSPI &tft);
