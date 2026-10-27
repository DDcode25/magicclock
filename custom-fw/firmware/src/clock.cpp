#include "clock.h"
#include <time.h>
#include "fonts.h"
#include "net.h"
#include "store.h"

namespace clockapp {

static uint16_t hourColor = TFT_WHITE, minuteColor = 0xFDC0, secondColor = 0xFAC0;
static bool hour12 = false;
static int dateFormat = 1;
static int shownMinute = -1, shownDay = -1;

static const char *const WEEKDAYS[] = {"неділя", "понеділок", "вівторок", "середа", "четвер", "п’ятниця", "субота"};

static uint16_t parseColor(const String &hex, uint16_t def) {
  if (hex.length() != 7 || hex[0] != '#') return def;
  long v = strtol(hex.c_str() + 1, nullptr, 16);
  return ((v >> 8) & 0xF800) | ((v >> 5) & 0x07E0) | ((v & 0xFF) >> 3);  // RGB888 -> RGB565
}

static void loadSettings() {
  hourColor = parseColor(store::getStr("/.sys/timecolor.json", "hc", "#FFFFFF"), TFT_WHITE);
  minuteColor = parseColor(store::getStr("/.sys/timecolor.json", "mc", "#FEBA01"), 0xFDC0);
  secondColor = parseColor(store::getStr("/.sys/timecolor.json", "sc", "#FF5900"), 0xFAC0);
  hour12 = store::getInt("/.sys/hour12.json", "h", 0) == 1;
  dateFormat = constrain(store::getInt("/.sys/day.json", "day", 1), 1, 5);
}

static String two(int v) { return v < 10 ? "0" + String(v) : String(v); }

static String dateText(const struct tm &t) {
  String d = two(t.tm_mday), m = two(t.tm_mon + 1), y = String(t.tm_year + 1900);
  switch (dateFormat) {
    case 2: return y + "." + m + "." + d;
    case 3: return m + "." + d + "." + y;
    case 4: return m + "." + d;
    case 5: return d + "." + m;
    default: return d + "." + m + "." + y;
  }
}

// HH:MM centred, hours and minutes in their own colours.
static void drawTime(TFT_eSPI &tft, const struct tm &t) {
  int h = t.tm_hour;
  if (hour12) h = h % 12 == 0 ? 12 : h % 12;
  String hh = two(h), mm = two(t.tm_min);
  useFont(tft, FONT_CLOCK_BIG);
  int w = tft.textWidth(hh + ":" + mm), x = (240 - w) / 2;
  tft.fillRect(0, 74, 240, 74, TFT_BLACK);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(hourColor, TFT_BLACK);
  tft.drawString(hh, x, 110);
  x += tft.textWidth(hh);
  tft.drawString(":", x, 110);
  x += tft.textWidth(":");
  tft.setTextColor(minuteColor, TFT_BLACK);
  tft.drawString(mm, x, 110);
}

static void drawDay(TFT_eSPI &tft, const struct tm &t) {
  tft.fillRect(0, 18, 240, 40, TFT_BLACK);
  tft.fillRect(0, 200, 240, 40, TFT_BLACK);
  useFont(tft, FONT_LARGE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(WEEKDAYS[t.tm_wday], 120, 38);
  tft.drawString(dateText(t), 120, 220);
}

static void drawSeconds(TFT_eSPI &tft, const struct tm &t) {
  useFont(tft, FONT_CLOCK_SEC);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(secondColor, TFT_BLACK, true);  // fill the glyph background: no flicker
  tft.setTextPadding(tft.textWidth("00") + 4);
  tft.drawString(two(t.tm_sec), 120, 172);
  tft.setTextPadding(0);
}

void draw(TFT_eSPI &tft) {
  loadSettings();
  tft.fillScreen(TFT_BLACK);
  shownMinute = shownDay = -1;
  tick(tft);
}

void tick(TFT_eSPI &tft) {
  if (!net::timeValid()) {
    if (shownMinute != -2) {
      tft.fillScreen(TFT_BLACK);
      useFont(tft, FONT_LARGE);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      tft.drawString("синхронізація часу…", 120, 120);
      shownMinute = -2;
    }
    return;
  }
  if (shownMinute == -2) {
    tft.fillScreen(TFT_BLACK);
    shownMinute = -1;
  }
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  if (t.tm_yday != shownDay) {
    shownDay = t.tm_yday;
    drawDay(tft, t);
  }
  if (t.tm_min != shownMinute) {
    shownMinute = t.tm_min;
    drawTime(tft, t);
  }
  drawSeconds(tft, t);
}

}  // namespace clockapp
