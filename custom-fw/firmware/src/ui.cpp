#include "ui.h"
#include <TFT_eSPI.h>
#include <time.h>
#include "alerts.h"
#include "apps.h"
#include "board.h"
#include "fonts.h"
#include "net.h"
#include "pictures.h"
#include "store.h"

namespace ui {

static TFT_eSPI tft;
static int current = 0;
static bool dirty = true;
// Measured on the device: idle ~101, finger 79..93 (a touch lowers the reading by only 10-20%).
static const int TOUCH_PRESS_PCT = 92, TOUCH_RELEASE_PCT = 95;
static int touchThreshold = 93, touchBase = 0;
static float touchBaseF = 0;
static uint32_t touchReadyAt = 0;  // ignore the sensor right after calibration
static volatile int touchRaw = 0;
static uint32_t taps = 0, doubleTaps = 0, longPresses = 0;

static const uint32_t TOUCH_SAMPLE_MS = 20;
static const uint32_t LONG_PRESS_MS = 800;
static const uint32_t DOUBLE_TAP_MS = 350;  // stock: double tap = previous screen

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
  useFont(tft, FONT_LARGE);
  tft.drawString("Не торкайтеся", 120, 105);
  tft.drawString("кнопки зверху!", 120, 135);
  delay(300);
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += touchRead(PIN_TOUCH);
    delay(20);
  }
  int base = sum / 16;
  if (base < 30) base = 100;  // someone was touching during boot; use the typical idle value
  touchBase = base;
  touchBaseF = base;
  touchThreshold = base * TOUCH_PRESS_PCT / 100;
  touchReadyAt = millis() + 1000;
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

// Long press. Pictures: next file. Other apps get an inner mode in later stages.
void enter() {
  if (current == APP_PICTURE) return pictures::next(tft);
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

String touchJson() {
  return String("{\"raw\":") + touchRaw + ",\"base\":" + touchBase + ",\"threshold\":" + touchThreshold +
         ",\"taps\":" + taps + ",\"double\":" + doubleTaps +
         ",\"long\":" + longPresses + "}";
}

void message(const String &title, const String &text) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  useFont(tft, FONT_LARGE);
  tft.drawString(title, 120, 100);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  useFont(tft, FONT_SMALL);
  tft.drawString(text, 120, 140);
}

// Device info, like the stock Settings screen: firmware, Wi-Fi mode, SSID, IP.
static void drawSettings() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  useFont(tft, FONT_LARGE);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString("Magic Box FW", 120, 40);
  useFont(tft, FONT_SMALL);
  const char *labels[] = {"Версія", "Wi-Fi", "Мережа", "IP"};
  String values[] = {FW_VERSION, net::connected() ? "клієнт" : net::apActive() ? "точка доступу" : "немає",
                     net::connected() ? net::ssid() : net::apActive() ? String(AP_SSID) : String("-"), net::ip()};
  for (int i = 0; i < 4; i++) {
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(labels[i], 92, 90 + i * 30);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(values[i], 102, 90 + i * 30);
  }
}

// Placeholder screen until each app is implemented: name, clock, network.
static bool isPlaceholder() { return current != APP_ALERTS && current != APP_PICTURE && current != APP_SETTINGS; }

static void drawApp() {
  static int shown = -1;
  if (shown == APP_PICTURE && current != APP_PICTURE) pictures::hide();
  shown = current;
  if (current == APP_ALERTS) return alerts::draw(tft);
  if (current == APP_PICTURE) return pictures::show(tft);
  if (current == APP_SETTINGS) return drawSettings();
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, 240, 36, 0x18E3);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_ORANGE, 0x18E3);
  useFont(tft, FONT_LARGE);
  tft.drawString(APP_ITEMS[current].label, 120, 19);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  useFont(tft, FONT_SMALL);
  tft.drawString("незабаром", 120, 160);
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
  useFont(tft, FONT_BUILTIN);
  tft.drawString(buf, 120, 105, 7);
  tft.setTextColor(net::connected() ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
  String status = net::connected() ? net::ip() : (net::apActive() ? String("Точка доступу ") + net::ip() : String("немає мережі"));
  tft.fillRect(0, 210, 240, 22, TFT_BLACK);
  tft.setTextPadding(0);
  useFont(tft, FONT_SMALL);
  tft.drawString(status, 120, 220);
}

// Stock controls: tap = next app, double tap = previous app, long press = enter().
// A single tap waits DOUBLE_TAP_MS for a second one before acting.
static void handleTouch() {
  static uint32_t lastSample = 0, pressStart = 0, tapAt = 0;
  static bool pressed = false, longFired = false, tapPending = false;
  if (tapPending && millis() - tapAt > DOUBLE_TAP_MS) {
    tapPending = false;
    next(1);
  }
  if (millis() - lastSample < TOUCH_SAMPLE_MS || millis() < touchReadyAt) return;
  lastSample = millis();
  // Average of the last 3 readings; press below 92 % of idle, release above 95 % (hysteresis).
  static uint16_t hist[3] = {0, 0, 0};
  static uint8_t hi = 0;
  hist[hi++ % 3] = touchRead(PIN_TOUCH);
  touchRaw = hist[2] ? (hist[0] + hist[1] + hist[2]) / 3 : hist[(hi - 1) % 3];
  bool down = pressed ? touchRaw < touchBase * TOUCH_RELEASE_PCT / 100 : touchRaw < touchThreshold;
  if (!pressed && !down) {  // idle: let the baseline follow slow drift
    touchBaseF += (touchRaw - touchBaseF) / 64;
    touchBase = (int)(touchBaseF + 0.5f);
    touchThreshold = touchBase * TOUCH_PRESS_PCT / 100;
  }
  if (down && !pressed) {
    pressed = true;
    longFired = false;
    pressStart = millis();
  } else if (down && pressed && !longFired && millis() - pressStart > LONG_PRESS_MS) {
    longFired = true;
    tapPending = false;
    longPresses++;
    enter();
  } else if (!down && pressed) {
    pressed = false;
    if (longFired || millis() - pressStart <= 40) return;
    if (tapPending) {
      tapPending = false;
      doubleTaps++;
      next(-1);
    } else {
      taps++;
      tapPending = true;
      tapAt = millis();
    }
  }
}

void loop() {
  handleTouch();

  // A new alert in the home region brings the alert map up (it blinks there).
  if (alerts::homeAlertStarted() && settings.appEnabled[APP_ALERTS] && current != APP_ALERTS) {
    current = APP_ALERTS;
    dirty = true;
  }

  static uint32_t lastSecond = 0;
  if (dirty) {
    dirty = false;
    drawApp();
    lastSecond = 0;
  }
  if (current == APP_ALERTS) alerts::loop(tft);
  if (current == APP_PICTURE) pictures::loop(tft);
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
