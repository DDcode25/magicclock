#include "fw_version.h"
#include "ui.h"
#include <TFT_eSPI.h>
#include <time.h>
#include "alerts.h"
#include "apps.h"
#include "board.h"
#include "clock.h"
#include "fonts.h"
#include "net.h"
#include "outage.h"
#include "store.h"
#include "weather.h"

namespace ui {

static TFT_eSPI tft;
static bool msgShown = false;

static int current = 0;
static bool dirty = true;
static uint32_t shownSince = 0;  // when the current app came up (auto rotation)

// Backlight is active-low: duty 255 = off, 0 = full brightness.
static void setBacklight(int pct) {
  int level = pct <= 0 ? 0 : map(pct, 1, 100, 4, 255);
  analogWrite(PIN_BACKLIGHT, 255 - level);
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

// System messages (boot, restart, update): large bold text, word-wrapped to the screen width and
// centred as one block. Each part starts on a new line in its own colour.
struct Part {
  const char *text;
  uint16_t color;
};

static void drawBlock(std::initializer_list<Part> parts) {
  static const int LINE_H = 36, MAX_LINES = 6;
  String lines[MAX_LINES];
  uint16_t colors[MAX_LINES];
  int n = 0;
  useFont(tft, FONT_HUGE);
  for (const Part &p : parts) {
    String rest = p.text, line;
    while (rest.length() && n < MAX_LINES) {
      int sp = rest.indexOf(' ');
      String word = sp < 0 ? rest : rest.substring(0, sp);
      rest = sp < 0 ? String() : rest.substring(sp + 1);
      String tryLine = line.length() ? line + " " + word : word;
      if (line.length() && tft.textWidth(tryLine) > 232) {
        lines[n] = line, colors[n++] = p.color;
        line = word;
      } else {
        line = tryLine;
      }
    }
    if (line.length() && n < MAX_LINES) lines[n] = line, colors[n++] = p.color;
  }
  tft.setTextDatum(MC_DATUM);
  int y = 120 - (n - 1) * LINE_H / 2;
  for (int i = 0; i < n; i++, y += LINE_H) {
    useFont(tft, FONT_HUGE);  // a single word wider than the screen: next smaller font
    if (tft.textWidth(lines[i]) > 236) useFont(tft, FONT_LARGE);
    if (tft.textWidth(lines[i]) > 236) useFont(tft, FONT_SMALL);
    tft.setTextColor(colors[i], TFT_BLACK);
    tft.drawString(lines[i], 120, y);
  }
}

// Position of app index k in APP_ORDER.
static int orderPos(int k) {
  for (int i = 0; i < APP_COUNT; i++)
    if (APP_ORDER[i] == k) return i;
  return 0;
}

// First enabled app at or after app `from` going `dir` (+1 / -1) through APP_ORDER, wrapping.
static int firstEnabled(int from, int dir) {
  int p = orderPos(from);
  for (int i = 0; i < APP_COUNT; i++) {
    int k = APP_ORDER[((p + dir * i) % APP_COUNT + APP_COUNT) % APP_COUNT];
    if (settings.appEnabled[k]) return k;
  }
  return APP_ORDER[0];
}

void begin() {
  analogWriteRange(255);
  analogWriteFreq(5000);
  pinMode(PIN_BACKLIGHT, OUTPUT);
  setBacklight(0);  // keep dark while the panel initializes
  tft.init();
  tft.fillScreen(TFT_BLACK);
  setBacklight(settings.brightness);
  current = firstEnabled(settings.bootApp >= 0 ? settings.bootApp : APP_ORDER[0], 1);
  dirty = true;
}

// No touch to find the address: after boot the network and IP stay up for a few seconds.
static uint32_t ipUntil = 0;
static void drawSettings();
void showIp() {
  drawSettings();
  msgShown = true;
  ipUntil = millis() + 8000;
}

void applySettings() {
  msgShown = false;
  if (!settings.appEnabled[current]) current = firstEnabled(current, 1);
  setBacklight(effectiveBrightness());
  dirty = true;
}

void next(int dir) {
  msgShown = false;
  int step = dir >= 0 ? 1 : -1;
  current = firstEnabled(APP_ORDER[(orderPos(current) + step + APP_COUNT) % APP_COUNT], step);
  dirty = true;
}

// /set?enter (long press on the SmallTV): outage today <-> tomorrow.
void enter() {
  if (current == APP_OUTAGE) return outagescreen::toggleDay(tft);
  dirty = true;
}

bool open(const String &name) {
  for (int i = 0; i < APP_COUNT; i++) {
    if (name.equalsIgnoreCase(appName(i)) || name.equalsIgnoreCase(appKey(i))) {
      if (!settings.appEnabled[i]) return false;
      msgShown = false;
      current = i;
      dirty = true;
      return true;
    }
  }
  return false;
}

String stateJson() {
  return String(F("{\"app\":\"")) + appKey(current) + F("\",\"message\":") + (msgShown ? 1 : 0) + '}';
}

static void drawMessage(const char *title, const char *text) {
  tft.fillScreen(TFT_BLACK);
  drawBlock({{title, TFT_ORANGE}, {text, TFT_WHITE}});
}

// Single-threaded: drawn right away, also from web handlers; stays up until the next app change.
void message(const String &title, const String &text) {
  drawMessage(title.c_str(), text.c_str());
  msgShown = true;
  ipUntil = 0;
}

// Device info, like the stock Settings screen: firmware, Wi-Fi mode, SSID, IP.
static void drawSettings() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  useFont(tft, FONT_LARGE);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString(F("UA Smart Box"), 120, 40);
  useFont(tft, FONT_SMALL);
  const __FlashStringHelper *labels[] = {F("Версія"), F("Wi-Fi"), F("Мережа"), F("IP")};
  String values[] = {F(FW_VERSION), net::connected() ? F("клієнт") : net::apActive() ? F("точка доступу") : F("немає"),
                     net::connected() ? net::ssid() : net::apActive() ? String(AP_SSID) : String('-'), net::ip()};
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
static bool isPlaceholder() {
  return current != APP_ALERTS && current != APP_SETTINGS && current != APP_CLOCK && current != APP_WEATHER &&
         current != APP_OUTAGE;
}

static void drawApp() {
  static int shown = -1;
  if (shown == APP_ALERTS && current != APP_ALERTS) alerts::hide();
  shown = current;
  shownSince = millis();
  if (current == APP_ALERTS) return alerts::draw(tft);
  if (current == APP_SETTINGS) return drawSettings();
  if (current == APP_CLOCK) return clockapp::draw(tft);
  if (current == APP_WEATHER) return weather::draw(tft);
  if (current == APP_OUTAGE) return outagescreen::draw(tft);
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, 240, 36, 0x18E3);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_ORANGE, 0x18E3);
  useFont(tft, FONT_LARGE);
  tft.drawString(appLabel(current), 120, 19);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  useFont(tft, FONT_SMALL);
  tft.drawString(F("незабаром"), 120, 160);
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
  String status = net::connected() ? net::ip() : (net::apActive() ? String(F("Точка доступу ")) + net::ip() : String(F("немає мережі")));
  tft.fillRect(0, 210, 240, 22, TFT_BLACK);
  tft.setTextPadding(0);
  useFont(tft, FONT_SMALL);
  tft.drawString(status, 120, 220);
}

void loop() {
  if (ipUntil && (int32_t)(millis() - ipUntil) >= 0) {  // boot info shown long enough
    ipUntil = 0;
    msgShown = false;
    dirty = true;
  }
  if (msgShown) return;  // keep the notice up (e.g. during OTA)

  // Auto rotation (setting): the enabled apps take turns, each for its own time (not while the alert
  // map is up because of an alert).
  static int autoReturnApp = -1;
  int sec = settings.appSec[current];
  if (settings.autoNext && sec > 0 && autoReturnApp < 0 && !alerts::blinkingNow() && millis() - shownSince > (uint32_t)sec * 1000) {
    int nextApp = firstEnabled(APP_ORDER[(orderPos(current) + 1) % APP_COUNT], 1);
    if (nextApp != current) current = nextApp, dirty = true;
    else shownSince = millis();
  }

  // A new alert in the home region brings the alert map up (it blinks there) and keeps it. With the
  // "auto" setting any change in the alerts shows the map for a minute, then the previous app is back
  // (unless the user switched apps meanwhile).
  static uint32_t autoUntil = 0;
  bool homeAlert = alerts::homeAlertStarted(), changed = alerts::alertsChanged();
  if (autoReturnApp >= 0 && current != APP_ALERTS) autoReturnApp = -1;  // switched by touch / web
  if (settings.appEnabled[APP_ALERTS]) {
    if (homeAlert) {
      autoReturnApp = -1;
      if (current != APP_ALERTS) current = APP_ALERTS, dirty = true;
    } else if (changed && (current != APP_ALERTS || autoReturnApp >= 0)) {
      if (current != APP_ALERTS) autoReturnApp = current, current = APP_ALERTS, dirty = true;
      autoUntil = millis() + 60000;
    }
  }
  if (autoReturnApp >= 0 && (int32_t)(millis() - autoUntil) >= 0) {
    current = settings.appEnabled[autoReturnApp] ? autoReturnApp : current;
    autoReturnApp = -1;
    dirty = true;
  }

  static uint32_t lastSecond = 0;
  if (dirty) {
    dirty = false;
    drawApp();
    lastSecond = 0;
  }
  if (current == APP_ALERTS) alerts::loop(tft);
  if (millis() - lastSecond >= 1000) {
    lastSecond = millis();
    if (isPlaceholder()) drawStatus();
    else if (current == APP_ALERTS) alerts::tick(tft);
    else if (current == APP_CLOCK) clockapp::tick(tft);
    else if (current == APP_WEATHER) weather::tick(tft);
    else if (current == APP_OUTAGE) outagescreen::tick(tft);
    static int lastBrightness = -1;
    int b = effectiveBrightness();
    if (b != lastBrightness) {
      lastBrightness = b;
      setBacklight(b);
    }
  }
}

}  // namespace ui
