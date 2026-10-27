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

void wifiMarkTick(TFT_eSPI &tft) {
  static bool shown = false;
  shown = !net::connected() || !shown;
  wifiMark(tft, shown);
}
