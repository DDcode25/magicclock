#include "ui.h"
#include <TFT_eSPI.h>
#include <time.h>
#include "alerts.h"
#include "apps.h"
#include "board.h"
#include "net.h"
#include "store.h"

namespace ui {

static TFT_eSPI tft;
static int current = 0;
static bool dirty = true;
static int touchThreshold = 60;

static const uint32_t TOUCH_SAMPLE_MS = 20;
static const uint32_t LONG_PRESS_MS = 800;

// Backlight is active-low: duty 255 = off, 0 = full brightness.
static void setBacklight(int pct) {
  int level = pct <= 0 ? 0 : map(pct, 1, 100, 4, 255);
  ledcWrite(BL_CHANNEL, 255 - level);
}

static int effectiveBrightness() {
  if (!settings.nightEnabled || !net::timeValid()) return settings.brightness;
  struct tm t;
  time_t now = time(nullptr);
  localtime_r(&now, &t);
  int h = t.tm_hour, a = settings.nightFrom, b = settings.nightTo;
  bool night = a <= b ? (h >= a && h < b) : (h >= a || h < b);
  return night ? settings.nightBrightness : settings.brightness;
}

static void calibrateTouch() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Don't Touch Top!", 120, 120, 4);
  delay(300);
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += touchRead(PIN_TOUCH);
    delay(20);
  }
  int base = sum / 16;
  if (base < 30) base = 100;  // someone was touching during boot; use the typical idle value
  touchThreshold = base * 2 / 3;
  Serial.printf("Touch: base %d, threshold %d\n", base, touchThreshold);
}

static int firstEnabled(int from, int dir) {
  for (int i = 0; i < APP_COUNT; i++) {
    int k = ((from + dir * i) % APP_COUNT + APP_COUNT) % APP_COUNT;
    if (settings.appEnabled[k]) return k;
  }
  return 0;
}

void begin() {
  ledcSetup(BL_CHANNEL, 5000, 8);
  ledcAttachPin(PIN_BACKLIGHT, BL_CHANNEL);
  setBacklight(0);  // keep dark while the panel initializes, like stock
  tft.init();
  tft.fillScreen(TFT_BLACK);
  setBacklight(settings.brightness);
  calibrateTouch();
  current = firstEnabled(settings.bootApp >= 0 ? settings.bootApp : 0, 1);
  dirty = true;
}

void applySettings() {
  if (!settings.appEnabled[current]) current = firstEnabled(current, 1);
  setBacklight(effectiveBrightness());
  dirty = true;
}

void next(int dir) {
  current = firstEnabled(current + (dir >= 0 ? 1 : -1), dir >= 0 ? 1 : -1);
  dirty = true;
}

void enter() {
  // Apps get an inner mode in later stages; for now just redraw.
  dirty = true;
}

bool open(const String &name) {
  for (int i = 0; i < APP_COUNT; i++) {
    if (name.equalsIgnoreCase(APP_ITEMS[i].name) || name.equalsIgnoreCase(APP_ITEMS[i].key)) {
      if (!settings.appEnabled[i]) return false;
      current = i;
      dirty = true;
      return true;
    }
  }
  return false;
}

void message(const String &title, const String &text) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString(title, 120, 100, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(text, 120, 140, 2);
}

// Placeholder screen until each app is implemented: name, clock, network.
static bool isPlaceholder() { return current != APP_ALERTS; }

static void drawApp() {
  if (current == APP_ALERTS) return alerts::draw(tft);
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, 240, 36, 0x18E3);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_ORANGE, 0x18E3);
  tft.drawString(APP_ITEMS[current].label, 120, 18, 4);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("coming soon", 120, 160, 2);
}

static void drawStatus() {
  char buf[16] = "--:--:--";
  if (net::timeValid()) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    strftime(buf, sizeof buf, "%H:%M:%S", &t);
  }
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextPadding(230);
  tft.drawString(buf, 120, 105, 7);
  tft.setTextColor(net::connected() ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
  String status = net::connected() ? net::ip() : (net::apActive() ? String("AP ") + AP_SSID + " " + net::ip() : String("offline"));
  tft.drawString(status, 120, 220, 2);
  tft.setTextPadding(0);
}

static void handleTouch() {
  static uint32_t lastSample = 0, pressStart = 0;
  static bool pressed = false, longFired = false;
  if (millis() - lastSample < TOUCH_SAMPLE_MS) return;
  lastSample = millis();
  bool down = touchRead(PIN_TOUCH) < touchThreshold;
  if (down && !pressed) {
    pressed = true;
    longFired = false;
    pressStart = millis();
  } else if (down && pressed && !longFired && millis() - pressStart > LONG_PRESS_MS) {
    longFired = true;
    enter();
  } else if (!down && pressed) {
    pressed = false;
    if (!longFired && millis() - pressStart > 40) next(1);
  }
}

void loop() {
  handleTouch();

  static uint32_t lastSecond = 0;
  if (dirty) {
    dirty = false;
    drawApp();
    lastSecond = 0;
  }
  if (millis() - lastSecond >= 1000) {
    lastSecond = millis();
    if (isPlaceholder()) drawStatus();
    else if (current == APP_ALERTS) alerts::tick(tft);
    static int lastBrightness = -1;
    int b = effectiveBrightness();
    if (b != lastBrightness) {
      lastBrightness = b;
      setBacklight(b);
    }
  }
}

}  // namespace ui
