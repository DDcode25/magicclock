#include "widgets.h"
#include "net.h"

void wifiMark(TFT_eSPI &tft, bool on) {
  const int cx = 229, cy = 13;
  tft.fillRect(cx - 9, cy - 10, 19, 12, TFT_BLACK);
  if (!on) return;
  uint16_t c = net::connected() ? 0x9CF3 : TFT_RED;
  tft.fillCircle(cx, cy - 1, 1, c);
  for (int r = 4; r <= 10; r += 3) tft.drawArc(cx, cy, r, r - 1, 135, 225, c, TFT_BLACK, false);
}

static int shown = -1;

static bool phaseOn() { return !net::connected() || (millis() / 2000) % 2 == 0; }  // 2 s on, 2 s off

void wifiMarkTick(TFT_eSPI &tft) {
  bool on = phaseOn();
  if (on == shown) return;
  shown = on;
  wifiMark(tft, on);
}

void wifiMarkRedraw(TFT_eSPI &tft) {
  shown = phaseOn();
  wifiMark(tft, shown);
}
